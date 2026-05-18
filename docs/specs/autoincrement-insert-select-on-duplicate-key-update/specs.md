# Autoincrement INSERT SELECT ON DUPLICATE KEY UPDATE

## Goal

Cover durable first-key `AUTO_INCREMENT` behavior for
`INSERT ... SELECT ... ON DUPLICATE KEY UPDATE` on MyLite-routed tables. This
slice focuses on the `SQLCOM_INSERT_SELECT` path because MariaDB supplies an
unknown row estimate to the handler, unlike multi-value `INSERT ... VALUES`.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_insert.cc:4137-4197` prepares `INSERT ... SELECT` through
  `mysql_insert_select_prepare()` and calls the same insert preparation path
  with `select_insert=true`.
- `mariadb/sql/sql_insert.cc:4226-4448` prepares `select_insert`, marks
  `DUP_UPDATE` statements with `HA_EXTRA_INSERT_WITH_UPDATE`, and calls
  `ha_start_bulk_insert((ha_rows) 0)` for unknown-row-count source inserts.
- `mariadb/sql/sql_insert.cc:4476-4529` stores each selected row into the
  insert table, dispatches `Write_record::write_record()`, and clears the
  autoincrement field between selected source rows.
- `mariadb/sql/sql_insert.cc:2214-2351` attempts `ha_write_row()` first, then
  uses the duplicate-update path and restores the statement-local generated
  cursor unless the duplicate-update branch explicitly changes the
  autoincrement column.
- `mariadb/sql/handler.cc:4432-4489` treats `estimation_rows_to_insert == 0`
  as unknown and reserves generated values in growing intervals starting with
  `AUTO_INC_DEFAULT_NB_ROWS`.
- `mariadb/sql/handler.cc:4368-4370` sets `AUTO_INC_DEFAULT_NB_ROWS` to `1`,
  so unknown source inserts reserve 1, then 2, then 4 values as the statement
  outgrows each interval.
- `mariadb/storage/innobase/handler/ha_innodb.cc:7776-7793` treats
  duplicate-key errors from `SQLCOM_INSERT_SELECT` as autoincrement-consuming
  attempts.
- `mariadb/storage/innobase/handler/ha_innodb.cc:16754-16902` advances
  InnoDB's persistent autoincrement state to the reserved interval boundary
  during `get_auto_increment()` under the default lock mode.
- `mariadb/storage/mylite/ha_mylite.cc:1805-1878` publishes MyLite durable
  first-key generated reservations from `get_auto_increment()` and marks them
  for rollback preservation.
- `mariadb/storage/mylite/ha_mylite.cc:1900-2005` calls
  `update_auto_increment()` before duplicate-key checks for generated insert
  rows, while explicit autoincrement insert values advance only after MyLite
  duplicate and FK checks pass.
- `mariadb/storage/mylite/ha_mylite.cc:2030-2130` applies duplicate-update
  rows through `update_row()` and advances explicit autoincrement updates after
  duplicate and FK checks.

## Scope

- Durable MyLite-routed first-key autoincrement tables requested as
  `ENGINE=InnoDB`.
- Ordered `INSERT ... SELECT ... ON DUPLICATE KEY UPDATE` rows with a duplicate
  selected row and enough generated selected rows to force unknown-row-count
  reservation growth.
- Duplicate-update branches that explicitly set the autoincrement column from
  an `INSERT ... SELECT` statement.
- Transaction rollback preservation and close/reopen persistence for the
  explicit duplicate-update branch.

## Non-Goals

- Exhaustive `INSERT ... SELECT` optimizer, trigger, view, partition, grouped
  later-in-key, offset/increment, integer-width, or `LAST_INSERT_ID()` matrices.
- Native InnoDB old-style autoincrement lock-mode parity.
- Binary log, replication, or wire-protocol behavior.
- Size-profile reduction work.

## Compatibility Impact

This slice broadens the representative ODKU claim from multi-value
`INSERT ... VALUES` to source-driven `INSERT ... SELECT`. For unknown source
row counts, the next durable generated value follows MariaDB's growing
reservation intervals, not only the number of successfully inserted rows. A
duplicate source row can reuse the statement-local generated cursor for later
successful rows, while the durable next value still resumes after the latest
reserved interval boundary.

Explicit high-value duplicate-update branches keep the existing MyLite update
semantics: successful high-value updates advance the durable next value and
preserve that advancement through transaction rollback and close/reopen.

The claim remains representative. Broader ODKU matrices remain planned for
grouped autoincrement, triggers, views, source errors, offset/increment, and
public insert-id behavior.

## Design

No production change is expected. MyLite already receives the SQL-layer
unknown-row-count reservations through `handler::update_auto_increment()`:

- `ha_mylite::get_auto_increment()` publishes the requested reservation
  interval boundary before row publication for durable first-key tables.
- `ha_mylite::write_row()` checks duplicates after generated reservation
  publication, allowing the SQL ODKU branch to handle the duplicate without
  undoing the reservation.
- `ha_mylite::update_row()` advances explicit autoincrement updates after
  MyLite duplicate-key and FK checks, so `INSERT ... SELECT` ODKU uses the same
  explicit-update path as `INSERT ... VALUES` ODKU.

## File Lifecycle

No file-format change is required. Durable state stays in the primary
`.mylite` file. Transaction rollback preservation uses the existing transaction
journal and autoincrement rollback-preservation marker.

## Embedded Lifecycle And API

No `libmylite` API change is required. The behavior is observable through
direct SQL execution. Public insert-id API coverage remains outside this slice.

## Storage-Engine Routing

The coverage uses requested `ENGINE=InnoDB`, which routes to MyLite storage in
the default embedded profile. Omitted/default, MyISAM, and Aria first-key
tables use the same durable MyLite path but are not repeated here.

## Build, Size, And Dependencies

No dependency, license, or intended size-profile change is introduced.

## Test Plan

- Add storage-engine smoke coverage for ordered
  `INSERT ... SELECT ... ON DUPLICATE KEY UPDATE` with a duplicate source row,
  proving visible generated ids and the durable tail gap after unknown-row-count
  reservation growth.
- Add storage-engine smoke coverage for an `INSERT ... SELECT` duplicate-update
  branch that explicitly sets a high autoincrement value.
- Cover transaction rollback preservation and close/reopen persistence for the
  explicit high-value duplicate-update branch.
- Run the focused storage-engine test, statement-rollback and transaction
  compatibility harness groups, shell syntax checks, `git diff --check`, and
  the dev, embedded-dev, and storage-smoke presets.

## Acceptance Criteria

- Ordered `INSERT ... SELECT` ODKU inserts generated rows with statement-local
  ids around a duplicate source row.
- The next generated statement resumes after MariaDB's unknown-row-count
  reservation boundary.
- Explicit high-value `INSERT ... SELECT` ODKU updates advance the next durable
  value, preserve that advancement through transaction rollback, and survive
  close/reopen.
- Roadmap and compatibility docs distinguish covered `INSERT ... VALUES` and
  `INSERT ... SELECT` ODKU behavior while leaving broader matrices planned.

## Risks And Open Questions

- `LAST_INSERT_ID()` / `mysql_insert_id()` behavior is not checked here because
  the public insert-id API surface is covered separately.
- Grouped later-in-key ODKU behavior may require storage-level prefix lookup
  work before it can be claimed broadly.
- Trigger, view, and source-error paths may have different SQL-layer ordering
  and remain planned.

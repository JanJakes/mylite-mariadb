# Autoincrement ON DUPLICATE KEY UPDATE

## Goal

Cover durable first-key `AUTO_INCREMENT` behavior for
`INSERT ... ON DUPLICATE KEY UPDATE` (ODKU) on MyLite-routed tables. The slice
focuses on generated-value reservation for duplicate rows and explicit
high-value updates performed by the duplicate-update branch.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_insert.cc:961-966` marks `DUP_UPDATE` inserts with
  `HA_EXTRA_INSERT_WITH_UPDATE` before row execution.
- `mariadb/sql/sql_insert.cc:2214-2351` first calls `ha_write_row()` and, on a
  duplicate-key result, evaluates the `ON DUPLICATE KEY UPDATE` list and calls
  `ha_update_row()` for the conflicting row.
- `mariadb/sql/sql_insert.cc:2313-2324` clears the generated insert id when a
  duplicate row is updated so the update branch behaves like an ordinary
  update for `LAST_INSERT_ID()` / `mysql_insert_id()`.
- `mariadb/sql/sql_insert.cc:2333-2349` restores the handler's statement-local
  generated-value cursor unless the duplicate-update branch explicitly updates
  the autoincrement column.
- `mariadb/sql/handler.cc:4432-4489` uses the multi-value row estimate from
  `thd->lex->many_values.elements` when reserving generated values.
- `mariadb/storage/innobase/handler/ha_innodb.cc:16754-16902` advances InnoDB's
  persistent autoincrement state to the reserved interval boundary in
  `get_auto_increment()` for non-old-style autoincrement locking.
- `mariadb/storage/innobase/handler/ha_innodb.cc:7710-7865` updates InnoDB's
  state after successful inserted or explicit autoincrement values, while
  duplicate insert handling has already consumed the generated reservation
  interval under the default lock mode.
- `mariadb/storage/mylite/ha_mylite.cc:1840-1878` publishes durable first-key
  generated reservations before row publication and marks them for rollback
  preservation.
- `mariadb/storage/mylite/ha_mylite.cc:1898-1950` calls
  `update_auto_increment()` before duplicate-key checks in `write_row()`.
- `mariadb/storage/mylite/ha_mylite.cc:2030-2130` applies duplicate-update rows
  through `update_row()` and advances explicit autoincrement values after
  duplicate/FK checks.

## Scope

- Durable MyLite-routed first-key autoincrement tables requested as
  `ENGINE=InnoDB`.
- Single-row duplicate `ON DUPLICATE KEY UPDATE` statements where the duplicate
  branch does not update the autoincrement column.
- Multi-row generated `ON DUPLICATE KEY UPDATE` statements with successful rows
  around a duplicate row, proving visible ids can be reused inside the
  statement while the next statement resumes after the reserved interval.
- Duplicate-update branches that explicitly set the autoincrement column to a
  high value.
- Close/reopen persistence and transaction rollback preservation for the
  explicit high-value duplicate-update path.

## Non-Goals

- Exhaustive ODKU expression, trigger, view, partition, source-error, or grouped
  later-in-key autoincrement matrices.
- Native InnoDB old-style autoincrement lock-mode parity.
- `LAST_INSERT_ID()` / `mysql_insert_id()` API coverage.
- Binary log, replication, or wire-protocol ODKU behavior.

## Compatibility Impact

The covered behavior moves durable routed tables closer to MariaDB/InnoDB's
non-gapless autoincrement behavior:

- duplicate-update rows consume generated reservation state even when the
  duplicate branch keeps the original autoincrement column value;
- successful non-duplicate rows around a duplicate-update row can reuse the
  statement-local generated cursor, while the next statement still starts after
  the reserved interval; and
- explicit high-value duplicate updates advance the durable next value and keep
  that advancement across transaction rollback and close/reopen.

The claim remains representative. Broader ODKU surfaces remain planned until
grouped-autoincrement, trigger, `LAST_INSERT_ID()`, offset, integer-width, and
error-path matrices are covered.

## Design

No production change is expected. MyLite already mirrors the relevant
MariaDB/InnoDB shape for durable first-key tables:

- `ha_mylite::get_auto_increment()` publishes the generated reservation lower
  bound before row execution, matching InnoDB's default persistent reservation
  behavior.
- `ha_mylite::write_row()` performs duplicate-key checks after reservation
  publication, letting the SQL layer enter the ODKU update branch without
  rolling back the generated reservation.
- `ha_mylite::update_row()` advances explicit autoincrement updates after
  duplicate and FK checks, so successful ODKU updates that set a high id reuse
  the existing durable update semantics.

## File Lifecycle

No file-format change is required. Durable state stays in the primary
`.mylite` file. The covered transaction rollback behavior uses existing
transaction journals and autoincrement rollback-preservation markers.

## Embedded Lifecycle And API

No `libmylite` API change is required. The behavior is observable through
direct SQL execution. The slice does not add insert-id API assertions.

## Storage-Engine Routing

The coverage uses requested `ENGINE=InnoDB`, which routes to MyLite storage in
the default embedded profile. Omitted/default, MyISAM, and Aria first-key
tables share the same durable MyLite path, but are not repeated in this slice.

## Build, Size, And Dependencies

No dependency, license, or intended size-profile change is introduced.

## Test Plan

- Add a storage-engine smoke test for single-row duplicate ODKU generated
  reservation, verifying the duplicate row updates in place and the next insert
  skips the generated attempt.
- Add a storage-engine smoke test for multi-row ODKU with successful generated
  rows around a duplicate-update row, verifying visible ids and the reserved
  tail gap.
- Add a storage-engine smoke test for duplicate-update branches that explicitly
  set a high autoincrement value, including transaction rollback and
  close/reopen persistence.
- Run the focused storage-engine test, statement-rollback and transaction
  compatibility harness groups, shell syntax checks, `git diff --check`, and
  the dev, embedded-dev, and storage-smoke presets.

## Acceptance Criteria

- Single-row duplicate ODKU updates the existing row and makes the next
  generated insert skip the duplicate insert attempt.
- Multi-row generated ODKU keeps successful row ids consistent with MariaDB's
  statement-local cursor behavior and preserves the reserved tail gap for the
  next statement.
- ODKU explicit high-value autoincrement updates advance the next value,
  preserve that advancement through rollback, and survive close/reopen.
- Roadmap and compatibility docs remove ODKU from the immediate
  autoincrement-gap TODO list while leaving broader ODKU matrices planned.

## Risks And Open Questions

- The slice does not compare `LAST_INSERT_ID()` / `mysql_insert_id()` because
  the current public API coverage for insert ids is separate from storage state.
- Grouped later-in-key autoincrement ODKU behavior may need storage-level
  prefix lookup before it can be claimed broadly.
- ODKU trigger, view, and source-error paths may have distinct SQL-layer
  ordering and remain planned.

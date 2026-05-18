# Autoincrement Reservation Gaps

## Goal

Persist durable first-key `AUTO_INCREMENT` reservation intervals when MariaDB
asks the MyLite handler for generated values. This extends failed-DML gap
coverage from row-attempt publication to InnoDB-style interval reservation for
multi-row generated inserts that reach MyLite `write_row()`.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.cc:handler::update_auto_increment()` passes
  `nb_desired_values`, `auto_increment_offset`, and
  `auto_increment_increment` into the storage-engine `get_auto_increment()`
  hook when a generated value is needed.
- `mariadb/sql/handler.cc:handler::update_auto_increment()` stores the value
  returned by `get_auto_increment()` into the row before the row reaches the
  engine write path, and advances the handler's in-statement cursor through
  the reserved interval.
- `mariadb/storage/innobase/handler/ha_innodb.cc:ha_innobase::get_auto_increment()`
  records `trx->n_autoinc_rows` from `nb_desired_values`, returns that count
  through `nb_reserved_values`, and updates InnoDB's table autoincrement
  counter before rows are committed when the autoincrement lock mode is not the
  old-style mode.
- `mariadb/storage/innobase/handler/ha_innodb.cc:innobase_next_autoinc()`
  computes the value after a reserved interval as
  `current + need * step`, rounded by the session offset/increment sequence.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::get_auto_increment()`
  currently reads durable state and returns `ULONGLONG_MAX` reserved values for
  first-key autoincrement. Durable state is advanced later from rows that reach
  `write_row()`, so failed multi-row statements only preserve values for rows
  that reached the handler write path.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()` now publishes
  advancing row-local autoincrement values before duplicate/FK checks and marks
  the checkpoint for rollback preservation. Reservation publication should use
  the same preservation marker.

## Scope

- For durable first-key MyLite-routed tables, publish the reserved
  autoincrement interval in `ha_mylite::get_auto_increment()`.
- Return the bounded reserved count to MariaDB instead of an unbounded
  `ULONGLONG_MAX` reservation for durable first-key generated values.
- Mark the active storage checkpoint after successful reservation publication
  so failed statement rollback preserves the reserved gap.
- Cover multi-row failed generated insert, default increment reservation,
  non-default offset/increment reservation, and close/reopen persistence.

## Non-Goals

- Grouped later-in-key autoincrement reservation. That path derives values from
  live index-entry prefix maxima and already advertises one reserved value at a
  time.
- MEMORY/HEAP volatile reservation persistence.
- Failed `UPDATE` autoincrement semantics.
- CHECK or other SQL-layer failures that occur before the engine write path
  asks for a generated value.
- Exhaustive integer-width overflow matrices for reserved intervals.
- Changing the storage file format.

## Compatibility Impact

Durable first-key generated inserts move closer to MariaDB/InnoDB behavior:
the reserved interval is persistent even if the statement later rolls back
earlier rows or fails before all reserved values are written. Successful
multi-row insert ids remain unchanged.

## Design

Add a MyLite helper equivalent to the InnoDB interval calculation for the
storage lower-bound model:

1. compute the first generated value with
   `mylite_first_auto_increment_value(next_value, offset, increment)`;
2. choose `reserved_values = nb_desired_values == 0 ? 1 : nb_desired_values`;
3. compute the durable next lower bound after the interval using
   `first_value + reserved_values * increment`, with overflow checks; and
4. publish that bound through `mylite_storage_advance_auto_increment()`.

For first-key durable tables, `ha_mylite::get_auto_increment()` should:

- publish and mark the interval only after it has a valid first value;
- return the bounded reservation count through `nb_reserved_values`;
- leave grouped later-in-key tables at a one-value reservation; and
- leave volatile MEMORY/HEAP tables on the current runtime-only path.

`ha_mylite::write_row()` keeps its row-local publication path for explicit
high values and for safety when `get_auto_increment()` did not reserve a
generated interval. For generated first-key rows after reservation, the later
row-local publication is a no-op because the reserved bound is already higher.

## File Lifecycle

No file-format change is required. Reservation state is an ordinary
autoincrement page in the primary `.mylite` file. If statement rollback
restores rows and index entries, the marked checkpoint republishes only
advancing autoincrement values for table IDs that existed at the checkpoint.

## Embedded Lifecycle And API

No `libmylite` API change is required. The behavior is exposed through SQL
inserts and existing storage state.

## Storage-Engine Routing

The behavior applies to durable MyLite-routed first-key autoincrement tables,
including requested `ENGINE=InnoDB` tables. Requested MyISAM/Aria first-key
tables also use the same durable MyLite table-local state. Grouped later-in-key
tables remain governed by the grouped-prefix path.

## Build, Size, And Dependencies

No dependency, license, or intended size-profile change is introduced. The
change is handler logic plus tests and documentation.

## Test Plan

- Add SQL storage-engine coverage for a multi-row generated insert that fails
  after reserving multiple values, proving the next insert starts after the
  interval MariaDB requested from the handler.
- Add coverage under non-default `auto_increment_offset` /
  `auto_increment_increment` to prove the stored lower bound rounds to the next
  session sequence value.
- Keep the existing failed duplicate, `INSERT IGNORE`, transaction rollback,
  offset/increment, and close/reopen tests green.
- Run focused storage-engine tests, statement-rollback and transaction harness
  groups, shell syntax checks, `git diff --check`, and the dev, embedded-dev,
  and storage-smoke presets.

## Acceptance Criteria

- Durable first-key multi-row generated insert failure preserves the requested
  reserved interval.
- Non-default offset/increment reservation persists the interval boundary and
  resumes at the next matching generated value.
- Successful multi-row generated insert ids remain unchanged.
- Grouped and volatile autoincrement behavior remains unchanged.
- Docs distinguish reservation gaps from SQL-layer failures that do not reach
  autoincrement generation, still-planned failed `UPDATE`, grouped
  reservation, and exhaustive matrix work.

## Risks And Open Questions

- `nb_desired_values` is exact for ordinary multi-row INSERT but can be zero or
  approximate for some source-driven statements. This slice uses MariaDB's
  bounded count when provided and reserves one value when the count is zero.
- Old-style InnoDB autoincrement lock mode updates after row attempts; MyLite
  targets the persistent default-style behavior because the project does not
  expose native InnoDB lock modes.

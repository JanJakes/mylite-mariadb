# Autoincrement Update Gaps

## Goal

Cover durable first-key `AUTO_INCREMENT` behavior when `UPDATE` statements
explicitly assign the autoincrement column. Successful updates to a higher
value should advance MyLite's durable next value, while failed or ignored
updates should not consume the attempted value.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_update.cc:2388-2433` fills the updated row image, calls the
  handler `ha_update_row()`, and treats duplicate-key errors as statement
  errors unless `UPDATE IGNORE` can skip the row.
- `mariadb/storage/innobase/handler/ha_innodb.cc:7978-8018` initializes the
  explicit-update autoincrement output from row-difference calculation to zero.
- `mariadb/storage/innobase/handler/ha_innodb.cc:8235-8239` records an
  explicit updated value only when the changed field is the table
  `AUTO_INCREMENT` column and the new value is not SQL `NULL`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:8536-8572` updates the InnoDB
  table autoincrement state only after `row_update_for_mysql()` returns
  `DB_SUCCESS` and an explicit autoincrement value was recorded.
- `mariadb/storage/mylite/ha_mylite.cc:2058-2094` checks duplicate keys and
  foreign-key constraints before publishing an autoincrement value from the
  updated row.
- `mariadb/storage/mylite/ha_mylite.cc:2097-2111` advances MyLite durable or
  volatile autoincrement state from the updated row after those checks pass.
- `mariadb/storage/mylite/ha_mylite.cc:6109-6138` maps a non-negative updated
  autoincrement field value to the durable lower bound `value + 1`, with
  `ULONGLONG_MAX` preserved as the read-failed sentinel.

## Scope

- Durable MyLite-routed first-key autoincrement tables, including requested
  `ENGINE=InnoDB` tables.
- Successful explicit high-value `UPDATE` of the autoincrement column.
- Failed duplicate-key `UPDATE` that attempts an explicit high value.
- Duplicate-key `UPDATE IGNORE` that attempts an explicit high value and skips
  the row.
- Transaction rollback of a successful explicit high-value `UPDATE`, proving
  MyLite preserves the advanced counter while restoring the row.
- Close/reopen persistence of the resulting next value.

## Non-Goals

- Generated autoincrement values in `UPDATE`; MariaDB does not set
  `table->next_number_field` for ordinary single-table updates.
- Grouped later-in-key autoincrement update matrices.
- MEMORY/HEAP volatile update persistence.
- Exhaustive integer-width or offset/increment matrices for updated values.
- Changing storage file format, public API, or handler transaction flags.

## Compatibility Impact

This pins MyLite to MariaDB/InnoDB-compatible explicit-update behavior for the
covered first-key shape:

- successful explicit higher values advance the next generated value;
- failed or ignored update attempts do not consume the attempted value; and
- transaction rollback restores rows while preserving already-successful
  autoincrement advancement for existing tables.

## Design

No production code change is expected. MyLite already checks duplicate and
foreign-key failure paths before `mylite_advance_auto_increment_from_row()` in
`ha_mylite::update_row()`, and transaction/savepoint rollback already preserves
advancing autoincrement pages for existing table IDs.

Add a focused storage-engine smoke test that:

1. creates a routed `ENGINE=InnoDB` table with an integer first-key
   `AUTO_INCREMENT` column and a unique title;
2. proves a failed duplicate-key `UPDATE` with a high explicit id leaves the
   next insert at the pre-failure generated value;
3. proves `UPDATE IGNORE` with the same shape also leaves the next generated
   value unchanged;
4. proves a successful explicit high-id update advances the next generated
   value;
5. rolls back a successful high-id update inside a transaction and proves the
   row is restored while the next generated value remains advanced; and
6. closes and reopens the database, then proves the advanced next value
   persists.

## File Lifecycle

No new companion file type is introduced. The behavior uses ordinary
autoincrement pages in the primary `.mylite` file plus the existing statement
and transaction checkpoint lifecycle.

## Embedded Lifecycle And API

No `libmylite` API change is required. The behavior is observable through
direct SQL execution.

## Storage-Engine Routing

The coverage uses requested `ENGINE=InnoDB`, which routes to MyLite storage in
the default embedded profile. Omitted/default, MyISAM, and Aria first-key
tables share the same durable MyLite state, but are not repeated in this
bounded slice.

## Build, Size, And Dependencies

No dependency, license, or intended size-profile change is introduced.

## Test Plan

- Add a storage-engine smoke test for failed, ignored, successful, rolled-back,
  and reopened explicit autoincrement updates.
- Run the focused storage-engine test, statement-rollback and transaction
  compatibility harness groups, shell syntax checks, `git diff --check`, and
  the dev, embedded-dev, and storage-smoke presets.

## Acceptance Criteria

- Failed duplicate-key `UPDATE` does not advance the next generated value.
- Duplicate-key `UPDATE IGNORE` does not advance the next generated value.
- Successful explicit high-value `UPDATE` advances the next generated value.
- Transaction rollback restores the updated row while preserving the advanced
  autoincrement value.
- Close/reopen resumes from the preserved value.
- Roadmap and compatibility docs remove failed `UPDATE` from the planned
  autoincrement gap list without claiming exhaustive update matrices.

## Risks And Open Questions

- This does not cover source-driven multi-table updates, generated columns,
  grouped later-in-key autoincrement, or every interaction with
  `auto_increment_offset` / `auto_increment_increment`.
- Failed row-publish I/O after autoincrement advancement remains a generic
  storage failure path rather than a SQL compatibility matrix.

# Autoincrement Grouped ODKU

## Goal

Cover `INSERT ... ON DUPLICATE KEY UPDATE` (ODKU) on routed tables whose
`AUTO_INCREMENT` column is a later part of a supported key, such as
`PRIMARY KEY (category, id)`.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_insert.cc:2214-2351` first calls `ha_write_row()` and, on a
  duplicate-key result, evaluates the ODKU update list and calls
  `ha_update_row()` for the conflicting row.
- `mariadb/sql/handler.cc:4432-4489` asks the handler for autoincrement values
  when the SQL layer needs a generated row value.
- `mariadb/storage/myisam/ha_myisam.cc:2380` and
  `mariadb/storage/maria/ha_maria.cc:3452` implement the native
  MyISAM/Aria grouped-prefix autoincrement model.
- `mariadb/storage/mylite/ha_mylite.h:95` advertises `HA_AUTO_PART_KEY`, so
  routed MyLite tables can accept later-in-key autoincrement definitions.
- `mariadb/storage/mylite/ha_mylite.cc:1805-1865` dispatches grouped
  autoincrement to `mylite_read_grouped_auto_increment()` and reserves one
  value at a time for grouped definitions.
- `mariadb/storage/mylite/ha_mylite.cc:3293-3365` reads live index entries for
  the grouped prefix and derives the next value from the current prefix maximum.

## Scope

- Durable MyLite-routed grouped autoincrement tables requested as `ENGINE=MyISAM`
  and `ENGINE=InnoDB`.
- Direct multi-row ODKU where successful inserted rows surround a duplicate
  update in the same prefix.
- Direct source-driven `INSERT ... SELECT` ODKU across multiple prefixes.
- Direct ODKU duplicate updates that call `LAST_INSERT_ID(id)`.
- Prepared `INSERT ... VALUES` and `INSERT ... SELECT` duplicate updates that
  call `LAST_INSERT_ID(id)`.
- Direct ODKU duplicate updates that explicitly move the autoincrement value to
  a higher value in the same prefix, including a source-driven update.
- Per-prefix close/reopen persistence.

## Non-Goals

- Exhaustive grouped ODKU expression, trigger, view, partition, source-error,
  offset/increment, integer-width, or prepared parameter-shape matrices.
- Native MyISAM/Aria sidecar storage behavior.
- Changing first-key autoincrement ODKU reservation behavior.
- Size-profile reduction work.

## Compatibility Impact

This narrows the broader ODKU matrix gap for grouped later-in-key
autoincrement. For grouped tables, generated values are derived from the live
maximum in the current prefix, so a duplicate-update attempt does not create a
table-local reserved tail gap the way first-key durable autoincrement ODKU does.
Explicit high-value duplicate updates still advance the current prefix because
the next generated value is derived from live rows.

The claim remains representative. Broader ODKU matrices stay planned for
triggers, views, offsets, integer widths, prepared parameter-shape expansion,
and additional error paths.

## Design

No production change is expected. Existing MyLite grouped autoincrement reads
the current prefix maximum through live index entries. The slice adds storage
engine smoke coverage that combines that path with MariaDB's existing ODKU
insert-then-update flow for direct `VALUES`, source-driven `INSERT ... SELECT`,
and representative prepared execution paths.

## File Lifecycle

No file-format or companion-file change is introduced. Durable table state
remains in the primary `.mylite` file and the test keeps the existing no
durable sidecar lifecycle check.

## Embedded Lifecycle And API

No `libmylite` API change is required. The behavior is observable through
direct and prepared SQL execution and public `mylite_changes()` /
`mylite_last_insert_id()` state after non-result statements.

## Storage-Engine Routing

Coverage uses requested `ENGINE=MyISAM` and `ENGINE=InnoDB`, which route to
MyLite storage in the storage-smoke profile. The behavior is MyLite's
grouped-prefix implementation; native MyISAM/InnoDB files are not created or
claimed.

## Build, Size, And Dependencies

No dependency, license, or intended size-profile change is introduced.

## Test Plan

- Add storage-engine smoke coverage for direct grouped ODKU with:
  - inserted rows around a duplicate update in the same prefix;
  - `LAST_INSERT_ID(id)` on a duplicate update;
  - an explicit high autoincrement update in the duplicate branch;
  - unaffected allocation in a different prefix; and
  - close/reopen allocation from live prefix maxima.
- Extend the smoke coverage with source-driven `INSERT ... SELECT` ODKU,
  prepared `VALUES` and `INSERT ... SELECT` `LAST_INSERT_ID(id)` duplicate
  updates, source-driven explicit high updates, and per-prefix close/reopen
  allocation after those variants.
- Run the focused storage-engine smoke test, storage-engine and
  statement-rollback compatibility harness groups, shell syntax checks, and
  `git diff --check`.

## Acceptance Criteria

- Multi-row grouped ODKU updates the duplicate row and allocates visible ids
  from the current prefix maximum.
- Source-driven grouped ODKU allocates inserted rows from each prefix's live
  maximum and updates duplicate rows in the same statement.
- The next grouped insert resumes from the live prefix maximum, not from a
  first-key-style reserved tail gap.
- Direct and prepared `LAST_INSERT_ID(id)` expose the duplicate row id for
  grouped ODKU.
- Direct and source-driven explicit high-value duplicate updates advance only
  their prefix.
- Close/reopen keeps grouped ODKU allocation based on durable live rows.

## Risks And Open Questions

- Native MyISAM/Aria/InnoDB server comparison is not part of this slice because
  MyLite routes those engine requests to MyLite storage and forbids durable
  sidecars.
- Grouped failed-update ODKU may have distinct SQL-layer ordering and remains
  planned.

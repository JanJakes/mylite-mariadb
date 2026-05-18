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

- Durable MyLite-routed grouped autoincrement tables requested as `ENGINE=MyISAM`.
- Direct multi-row ODKU where successful inserted rows surround a duplicate
  update in the same prefix.
- Direct ODKU duplicate updates that call `LAST_INSERT_ID(id)`.
- Direct ODKU duplicate updates that explicitly move the autoincrement value to
  a higher value in the same prefix.
- Per-prefix close/reopen persistence.

## Non-Goals

- Exhaustive grouped ODKU expression, trigger, view, partition, source-error,
  offset/increment, integer-width, or public prepared-statement matrices.
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
source-driven grouped ODKU, triggers, views, offsets, integer widths, and
additional error paths.

## Design

No production change is expected. Existing MyLite grouped autoincrement reads
the current prefix maximum through live index entries. The slice adds a storage
engine smoke test that combines that path with MariaDB's existing ODKU
insert-then-update flow.

## File Lifecycle

No file-format or companion-file change is introduced. Durable table state
remains in the primary `.mylite` file and the test keeps the existing no
durable sidecar lifecycle check.

## Embedded Lifecycle And API

No `libmylite` API change is required. The behavior is observable through
direct SQL execution and public `mylite_changes()` / `mylite_last_insert_id()`
state after direct statements.

## Storage-Engine Routing

Coverage uses requested `ENGINE=MyISAM`, which routes to MyLite storage in the
storage-smoke profile. The behavior is MyLite's grouped-prefix implementation;
native MyISAM files are not created or claimed.

## Build, Size, And Dependencies

No dependency, license, or intended size-profile change is introduced.

## Test Plan

- Add storage-engine smoke coverage for direct grouped ODKU with:
  - inserted rows around a duplicate update in the same prefix;
  - `LAST_INSERT_ID(id)` on a duplicate update;
  - an explicit high autoincrement update in the duplicate branch;
  - unaffected allocation in a different prefix; and
  - close/reopen allocation from live prefix maxima.
- Run the focused storage-engine smoke test, storage-engine and
  statement-rollback compatibility harness groups, shell syntax checks, and
  `git diff --check`.

## Acceptance Criteria

- Multi-row grouped ODKU updates the duplicate row and allocates visible ids
  from the current prefix maximum.
- The next grouped insert resumes from the live prefix maximum, not from a
  first-key-style reserved tail gap.
- `LAST_INSERT_ID(id)` exposes the duplicate row id for grouped ODKU.
- Explicit high-value duplicate updates advance only their prefix.
- Close/reopen keeps grouped ODKU allocation based on durable live rows.

## Risks And Open Questions

- Native MyISAM/Aria server comparison is not part of this slice because MyLite
  routes those engine requests to MyLite storage and forbids durable sidecars.
- Source-driven grouped ODKU and grouped failed-update ODKU may have distinct
  SQL-layer ordering and remain planned.

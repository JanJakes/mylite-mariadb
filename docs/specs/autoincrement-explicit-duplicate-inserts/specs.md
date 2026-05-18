# Autoincrement Explicit Duplicate Inserts

## Goal

Align durable first-key `AUTO_INCREMENT` handling for explicit high-value
duplicate inserts with MariaDB/InnoDB. Generated duplicate inserts still
preserve generated gaps, but explicit values that fail duplicate-key checks
must not advance the durable next value.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.cc:4387-4408` treats a nonzero autoincrement column
  value as explicit, adjusts only the handler's statement-local cursor, sets
  `insert_id_for_cur_row` to `0`, and returns without asking the engine to
  reserve generated values.
- `mariadb/sql/handler.cc:4591-4598` sets `next_insert_id` only for generated
  values.
- `mariadb/storage/innobase/handler/ha_innodb.cc:7718-7740` calls
  `update_auto_increment()` before the physical insert attempt.
- `mariadb/storage/innobase/handler/ha_innodb.cc:7776-7866` advances InnoDB's
  table autoincrement state on successful inserts or selected duplicate
  command shapes such as `REPLACE` / insert-select, but ordinary duplicate
  `SQLCOM_INSERT` failures do not go through `set_max_autoinc`.
- `mariadb/storage/mylite/ha_mylite.cc:1913-1954` uses
  `insert_id_for_cur_row` to identify generated values and keeps generated
  durable advancement before duplicate-key and foreign-key checks.
- `mariadb/storage/mylite/ha_mylite.cc:1956-2007` performs MyLite duplicate-key
  and child-FK checks before deferred explicit autoincrement advancement.

## Scope

- Durable MyLite-routed first-key autoincrement tables requested as
  `ENGINE=InnoDB`.
- Explicit high-value duplicate inserts that fail.
- Explicit high-value duplicate `INSERT IGNORE` statements that skip the row.
- Successful explicit high-value inserts, including transaction rollback and
  close/reopen persistence.
- Existing generated failed/ignored insert gap behavior must remain intact.

## Non-Goals

- `REPLACE`, `INSERT ... ON DUPLICATE KEY UPDATE`, insert-select duplicate
  modes, grouped later-in-key autoincrement, and MEMORY/HEAP behavior.
- Native InnoDB old-style autoincrement lock-mode parity.
- Storage I/O failure semantics after explicit advancement.

## Compatibility Impact

The slice narrows MyLite's failed-DML autoincrement claim:

- generated duplicate insert attempts keep MariaDB/InnoDB-style gaps;
- explicit high-value duplicate insert failures and ignored skips do not
  consume the attempted explicit high value; and
- successful explicit high-value inserts continue to advance durable state and
  preserve that advancement across transaction rollback and close/reopen.

Broader failed-DML matrices remain planned for insert-select, grouped-key,
offset/increment, integer-width, and storage-failure surfaces.

## Design

Use MariaDB's `insert_id_for_cur_row` signal to distinguish generated from
explicit insert attempts in `ha_mylite::write_row()`:

- keep the existing early durable advancement and rollback-preservation marker
  for generated values, because failed and ignored generated inserts are
  intentionally non-gapless;
- defer durable advancement for explicit autoincrement values until after
  duplicate-key and FK checks pass; and
- keep successful explicit insert advancement before row publication so
  transaction rollback can restore the row image while preserving the
  non-transactional next value.

Volatile MEMORY/HEAP rows already advance after duplicate checks and are not
changed by this slice.

## File Lifecycle

No file-format change is required. Durable state stays in the primary
`.mylite` file. Existing rollback-preservation markers continue to protect
generated gaps and successful explicit high-value advancement.

## Embedded Lifecycle And API

No `libmylite` API change is required. The behavior is observable through
direct SQL execution.

## Storage-Engine Routing

The coverage uses requested `ENGINE=InnoDB`, which routes to MyLite storage in
the default embedded profile. Omitted/default, MyISAM, and Aria first-key
tables share the same durable MyLite path, but are not repeated in this slice.

## Build, Size, And Dependencies

No dependency, license, or intended size-profile change is introduced.

## Test Plan

- Update failed-DML autoincrement storage-smoke coverage so explicit high-value
  duplicate insert failures and `INSERT IGNORE` skips do not advance the next
  generated value.
- Keep existing generated failed/ignored insert gap assertions.
- Add a successful explicit high-value insert assertion after the duplicate
  checks, including transaction rollback and close/reopen persistence.
- Run the focused storage-engine test, statement-rollback and transaction
  compatibility harness groups, shell syntax checks, `git diff --check`, and
  the dev, embedded-dev, and storage-smoke presets.

## Acceptance Criteria

- Generated duplicate insert attempts still preserve generated gaps.
- Explicit high-value duplicate insert failures do not consume attempted high
  values.
- Explicit high-value duplicate `INSERT IGNORE` skips do not consume attempted
  high values.
- Successful explicit high-value inserts still advance durable state through
  rollback and close/reopen.
- Roadmap and compatibility docs remove explicit duplicate-insert edge cases
  from the immediate autoincrement-gap TODO list while leaving broader matrices
  planned.

## Risks And Open Questions

- The slice does not cover explicit high-value duplicate behavior for
  insert-select or grouped later-in-key autoincrement tables.
- The handler still publishes successful explicit values before row
  publication; storage I/O failures after that point remain a future durability
  matrix.

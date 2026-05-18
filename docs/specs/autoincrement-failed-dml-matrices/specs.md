# Autoincrement Failed DML Matrices

## Goal

Broaden failed-DML `AUTO_INCREMENT` gap coverage beyond single-row failures.
The slice covers representative mixed-row statements where generated insert
reservation and update failure ordering are easy to overstate:

- mixed `INSERT IGNORE` can keep successful row ids consecutive while still
  preserving the reserved interval boundary for the next statement; and
- failed multi-row `UPDATE` attempts ordered to hit MyLite update checks before
  any prior high-value publication must not consume attempted explicit high
  values.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- MariaDB's InnoDB autoincrement documentation says persistent
  autoincrement is not transactional and names `INSERT IGNORE`, `ROLLBACK`,
  and `ROLLBACK TO SAVEPOINT` as cases that can leave gaps:
  <https://mariadb.com/docs/server/server-usage/storage-engines/innodb/auto_increment-handling-in-innodb>.
- `mariadb/sql/handler.cc:handler::update_auto_increment()` uses the
  multi-value statement estimate from `thd->lex->many_values.elements` when
  reserving generated insert values, so mixed `INSERT IGNORE` rows can reserve
  values even when some rows are skipped.
- `mariadb/storage/innobase/handler/ha_innodb.cc:7698-7860` updates InnoDB's
  table autoincrement state after successful inserts that used generated or
  explicit autoincrement values, with duplicate-key handling restricted to
  command shapes such as `REPLACE` and insert-select variants.
- `mariadb/sql/sql_update.cc:2388-2433` applies each updated row through
  `ha_update_row()` and stops on non-ignored row failures.
- `mariadb/storage/innobase/handler/ha_innodb.cc:8531-8603` records an
  explicit updated autoincrement value and advances InnoDB's persistent
  counter only after `row_update_for_mysql()` succeeds.
- `mariadb/storage/mylite/ha_mylite.cc:1860-1948` already publishes durable
  insert autoincrement values before duplicate/FK checks and marks the active
  checkpoint so top-level failed statement rollback preserves generated gaps.
- `mariadb/storage/mylite/ha_mylite.cc:2058-2111` checks duplicate/FK update
  failures before publishing explicit autoincrement advancement.
- `packages/mylite-storage/src/storage.c:3039-3051` exposes the statement
  checkpoint marker that tells rollback to keep advancing autoincrement pages.

## Scope

- Durable MyLite-routed first-key autoincrement tables, including requested
  `ENGINE=InnoDB`.
- Mixed generated `INSERT IGNORE` where successful rows surround a skipped
  duplicate row and the statement's reserved interval leaves a tail gap after
  the successful rows.
- Failed multi-row parent `UPDATE` that attempts explicit high values but is
  ordered to fail a foreign-key restriction before publishing advancement,
  proving the attempted high values are not consumed.
- Close/reopen persistence for the covered mixed-row insert next value.

## Non-Goals

- Exhaustive `INSERT ... ON DUPLICATE KEY UPDATE` behavior.
- Explicit high-value duplicate insert failures that never successfully publish
  a row.
- Grouped later-in-key autoincrement failed-DML matrices, which are covered
  separately in
  `docs/specs/autoincrement-grouped-failed-dml-matrices/specs.md`.
- MEMORY/HEAP runtime-volatile failed-DML gaps.
- Storage I/O failure semantics after autoincrement publication.
- Handler-level savepoint hooks, WAL, isolation, or lock-mode changes.

## Compatibility Impact

The covered durable first-key statements move closer to MariaDB/InnoDB's
non-gapless autoincrement behavior:

- a mixed `INSERT IGNORE` statement keeps the reserved interval boundary even
  when a skipped row does not itself take a visible generated id; and
- a failed multi-row `UPDATE` that does not pass MyLite FK checks before any
  high-value publication leaves attempted explicit high values unused.

The support claim remains representative. Broader failed-DML matrices stay
planned until prior-success multi-row update failures, broader
`ON DUPLICATE KEY UPDATE`, source-driven DML, and offset/increment
cross-products are covered. Grouped later-in-key failed-DML matrices are
covered separately in
`docs/specs/autoincrement-grouped-failed-dml-matrices/specs.md`.

## Design

No production code change is expected. Keep the existing insert path.
`ha_mylite::get_auto_increment()` publishes the first-key generated
reservation interval, and `ha_mylite::write_row()` marks the active statement
checkpoint after durable autoincrement publication. That is enough for mixed
generated `INSERT IGNORE` statements: successful rows can still receive
consecutive ids while the next statement resumes after the reserved interval.

Keep the existing durable `ha_mylite::update_row()` ordering for failed update
attempts: duplicate-key and foreign-key checks run before
`mylite_advance_auto_increment_from_row()`. The representative FK-protected
multi-row update is ordered so the failing row runs first, proving attempted
explicit high values remain unused when the statement fails before MyLite
publishes an updated row.

## File Lifecycle

No file-format change is required. The behavior uses ordinary autoincrement
pages in the primary `.mylite` file and the existing statement checkpoint
rollback lifecycle.

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

- Add a storage-engine smoke test for mixed generated `INSERT IGNORE` with
  successful rows around a skipped duplicate row and a reserved tail gap after
  the statement.
- Add a storage-engine smoke test for a failed ordered multi-row parent
  `UPDATE` that attempts explicit high values, hits a foreign-key restriction
  before any high-value publication, and proves the next generated value
  remains at the ordinary post-insert value.
- Reopen the database and prove the covered next value persists.
- Run the focused storage-engine test, statement-rollback and transaction
  compatibility harness groups, shell syntax checks, `git diff --check`, and
  the dev, embedded-dev, and storage-smoke presets.

## Acceptance Criteria

- Mixed generated `INSERT IGNORE` leaves visible rows around the skipped row
  and advances the next statement to the reserved interval boundary.
- Failed multi-row explicit autoincrement `UPDATE` restores row images and does
  not consume attempted high values when it fails before MyLite publishes an
  updated row.
- Failed duplicate/FK update attempts that do not pass MyLite checks still do
  not consume attempted values.
- Close/reopen resumes from the preserved mixed-row next value.
- Roadmap and compatibility docs narrow, but do not eliminate, the broader
  failed-DML autoincrement matrix gap.

## Risks And Open Questions

- Explicit high-value duplicate insert failures are covered separately in
  `docs/specs/autoincrement-explicit-duplicate-inserts/specs.md`; insert-select
  variants remain planned.
- Prior-success multi-row `UPDATE` failure is covered separately in
  `docs/specs/autoincrement-prior-success-failed-update/specs.md`.

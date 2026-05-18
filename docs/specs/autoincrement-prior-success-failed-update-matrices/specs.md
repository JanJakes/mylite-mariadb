# Autoincrement Prior-Success Failed Update Matrices

## Goal

Broaden prior-success failed `UPDATE` autoincrement coverage beyond one
foreign-key failure shape. A successful earlier row-level explicit
autoincrement advancement must remain durable when a later row in the same
statement fails through either handler-level duplicate-key checks or SQL-layer
CHECK constraints.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_update.cc:2388-2433` processes matching rows one at a time,
  verifies view/CHECK conditions before `ha_update_row()`, and then calls the
  handler update path for row publication.
- `mariadb/sql/table.cc:6576-6666` implements
  `TABLE_LIST::view_check_option()` and `TABLE::verify_constraints()`, returning
  `VIEW_CHECK_ERROR` for non-ignored CHECK failures before handler publication.
- `mariadb/sql/handler.cc:8373-8415` routes successful SQL-layer update rows
  through `handler::ha_update_row()`, then into the engine's `update_row()`.
- `mariadb/storage/mylite/ha_mylite.cc:2058-2111` performs MyLite duplicate and
  FK checks before publishing explicit autoincrement advancement for a row.
- `mariadb/storage/mylite/ha_mylite.cc:2112-2134` publishes explicit
  autoincrement advancement and marks the active checkpoint for preservation
  before appending the replacement row.
- `docs/specs/autoincrement-prior-success-failed-update/specs.md` covers the
  initial FK-protected prior-success case and the design for using the
  statement rollback autoincrement preservation marker.

## Scope

- Durable MyLite-routed first-key autoincrement tables requested as
  `ENGINE=InnoDB`.
- Ordered single-table multi-row `UPDATE` statements where the first row
  successfully changes the autoincrement column to a high value.
- Later-row failure through:
  - MyLite duplicate-key checks inside `ha_mylite::update_row()`.
  - MariaDB CHECK constraint verification before `ha_update_row()`.
- Close/reopen persistence for the preserved next values.

## Non-Goals

- Exhaustive trigger, view, generated-column, partition, offset/increment,
  integer-width, multi-table, or `UPDATE IGNORE` matrices. Representative
  grouped later-in-key failed-update matrices are covered separately in
  `docs/specs/autoincrement-grouped-failed-dml-matrices/specs.md`.
- New file format, public API, or storage checkpoint primitives.
- Size-profile reduction work.

## Compatibility Impact

The slice strengthens MyLite's MariaDB/InnoDB-compatible non-gapless
autoincrement claim for failed statements. Earlier successful explicit
autoincrement advancement in a multi-row update remains durable even when a
later row aborts the statement through a different failure surface. Row and
index visibility still roll back to the statement-start state.

This remains representative coverage. Broader update matrices for triggers,
views, generated columns, multi-table updates, and offset/increment variants
remain planned. Representative grouped autoincrement failed-update matrices
are covered separately in
`docs/specs/autoincrement-grouped-failed-dml-matrices/specs.md`.

## Design

No production design change is expected beyond the existing preservation marker
introduced by the prior-success FK slice. The tests intentionally exercise two
different later-failure positions:

1. Duplicate-key failure reaches `ha_mylite::update_row()` for the failing row
   but rejects before that row can publish its attempted high value.
2. CHECK failure aborts in the SQL layer before `ha_update_row()` for the
   failing row.

Both cases rely on the marker set by the earlier successful update row. That is
the behavior being verified.

## File Lifecycle

No file-format change is required. Autoincrement state remains stored in the
primary `.mylite` file. Statement rollback restores row/index visibility and
preserves only autoincrement pages explicitly marked for preservation.

## Embedded Lifecycle And API

No `libmylite` API change is required. The behavior is observable through
direct SQL execution and close/reopen lifecycle checks.

## Storage-Engine Routing

Coverage uses requested `ENGINE=InnoDB`, which routes to MyLite storage in the
default embedded profile. The durable MyLite update path is shared with
omitted/default, MyISAM, and Aria first-key tables, but those requested-engine
tokens are not repeated in this matrix.

## Build, Size, And Dependencies

No dependency, license, or intended size-profile change is introduced.

## Test Plan

- Add storage-engine smoke coverage for an ordered prior-success update where a
  later row fails a secondary unique-key update.
- Add storage-engine smoke coverage for an ordered prior-success update where a
  later row fails a CHECK constraint before handler publication.
- Verify the earlier high-id row images are rolled back, no high-id rows remain
  visible, and the next generated insert resumes after the earlier successful
  high value.
- Verify close/reopen persistence for both preserved next values.
- Run the focused storage-engine test, statement-rollback and transaction
  compatibility harness groups, shell syntax checks, `git diff --check`, and
  the dev, embedded-dev, and storage-smoke presets.

## Acceptance Criteria

- A later duplicate-key failure after a prior successful explicit high-value
  update preserves the prior advancement but not the failing row's attempted
  value.
- A later CHECK failure after a prior successful explicit high-value update
  preserves the prior advancement.
- Statement rollback restores row/index visibility in both cases.
- Close/reopen resumes from the preserved next values.
- Roadmap and compatibility docs distinguish this representative matrix from
  still-planned exhaustive update matrices.

## Risks And Open Questions

- The cases deliberately avoid triggers, views, generated columns, and
  multi-table updates because those may fail or publish rows through different
  SQL paths.
- This coverage does not prove offset or integer-width variants for
  prior-success update failures; grouped later-in-key behavior is covered
  separately in
  `docs/specs/autoincrement-grouped-failed-dml-matrices/specs.md`.

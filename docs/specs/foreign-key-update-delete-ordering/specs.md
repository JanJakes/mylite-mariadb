# Foreign-Key Update/Delete Ordering

## Problem

MyLite now covers ordered multi-row FK inserts for the supported
`RESTRICT` / `NO ACTION` subset, but update/delete ordering remains only broadly
described. The next useful slice is representative deterministic coverage for
self-referential update and delete statements where row order changes whether an
immediate parent check should pass or fail.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_update.cc` scans candidate rows and calls
  `handler::ha_update_row()` for each non-batched update. MyLite does not
  advertise bulk update support, so FK checks run once per updated row.
- `mariadb/sql/sql_delete.cc` calls `TABLE::delete_row()` for each deleted row,
  which delegates to `handler::ha_delete_row()`.
- `mariadb/sql/handler.cc:handler::ha_update_row()` and
  `handler::ha_delete_row()` delegate to the storage engine's row operation.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::update_row()` checks child
  FKs on the new row, then parent FKs on the old/new parent key before
  publishing the updated row.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::delete_row()` checks parent
  FKs before publishing the delete marker.
- `mariadb/storage/mylite/ha_mylite.cc:mylite_check_parent_foreign_key()` skips
  parent checks when the referenced key prefix is unchanged, otherwise it
  checks whether a child key prefix still exists in the current primary-file
  index state.

## Design

Keep update/delete behavior immediate, row-ordered, and non-deferrable:

1. A self-referential `DELETE ... ORDER BY` that tries to delete a parent before
   its child fails and statement rollback preserves both rows.
2. The same delete shape succeeds when deterministic `ORDER BY` deletes the
   child first and then the parent.
3. A self-referential `UPDATE ... ORDER BY` that tries to update a referenced
   parent key before clearing the child reference fails and preserves both rows.
4. The same update shape succeeds when deterministic `ORDER BY` clears the
   child reference first and then updates the parent key.
5. Do not implement deferred graph validation, cascades, `SET NULL`,
   `SET DEFAULT`, or multi-table update/delete FK ordering in this slice.

## Compatibility Impact

This documents a partial MySQL/MariaDB-compatible behavior for MyLite's current
FK subset: supported FK checks are immediate and see mutations already published
by earlier rows in the same statement. It does not claim SQL-standard deferrable
constraints or full application-wide FK graph reordering.

## Affected Subsystems

- `packages/libmylite/tests/embedded_storage_engine_test.c`: SQL regression
  coverage for deterministic self-referential update/delete order.
- Documentation and compatibility matrices: move representative
  self-referential update/delete ordering from planned to covered partial
  behavior.
- `mariadb/storage/mylite/ha_mylite.cc`: code changes only if tests reveal a
  mismatch.

## Single-File And Embedded Lifecycle

No file-format change is intended. The behavior uses existing row payload,
index-entry, FK metadata, and statement rollback mechanisms inside the primary
`.mylite` file. No durable sidecar or new runtime companion is introduced.

## Public API, Build, Size, And Dependencies

No public API, build-profile, size-profile, license, or dependency change is
intended.

## Test Plan

- Create self-referential rows with a parent and child.
- Verify parent-first ordered delete fails and both rows remain.
- Verify child-first ordered delete succeeds and both rows are gone.
- Create a second parent/child pair.
- Verify parent-first ordered update that changes the parent key and clears the
  child reference fails and preserves both rows.
- Verify child-first ordered update succeeds.
- Repeat representative visibility checks after close/reopen.
- Run focused storage-engine build/test targets, `ctest --preset
  storage-smoke-dev`, `ctest --preset dev`, and `git diff --check`.

## Acceptance Criteria

- Deterministic self-referential update/delete ordering has SQL-level coverage.
- Failed parent-first statements roll back prior statement work.
- Child-first ordered statements publish the expected row state and preserve it
  across reopen.
- Docs distinguish this representative coverage from broader multi-table,
  cascade, `SET NULL`, `SET DEFAULT`, and deferred validation work.

## Implementation Status

Implemented in this slice:

- SQL storage-engine coverage rejects parent-first self-referential ordered
  delete and verifies both rows remain.
- SQL storage-engine coverage accepts child-first self-referential ordered
  delete and verifies both rows are removed.
- SQL storage-engine coverage rejects parent-first self-referential ordered
  update that would change the referenced parent key before clearing the child
  reference.
- SQL storage-engine coverage accepts the matching child-first ordered update
  and verifies the new row state after close/reopen.

## Risks And Open Questions

- The tests intentionally use `ORDER BY` on single-table statements. They do not
  claim optimizer-independent order for unordered statements.
- Multi-table update/delete, non-self-referential parent/child pairs, cascades,
  `SET NULL`, and `SET DEFAULT` remain future slices.

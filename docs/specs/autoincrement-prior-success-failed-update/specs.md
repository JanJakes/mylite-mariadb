# Autoincrement Prior-Success Failed Update

## Goal

Preserve durable first-key `AUTO_INCREMENT` advancement when a multi-row
`UPDATE` successfully publishes an explicit high autoincrement value, then a
later row in the same statement fails and rolls back visible row changes.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_update.cc:2388-2433` applies matching update rows one at a
  time through `ha_update_row()` and stops on non-ignored row failures.
- `mariadb/sql/sql_update.cc:2495-2520` reports the handler error and aborts
  the statement after a row-level update failure.
- `mariadb/storage/innobase/handler/ha_innodb.cc:8531-8603` records explicit
  autoincrement updates and writes the persistent autoincrement state only
  after `row_update_for_mysql()` succeeds.
- `mariadb/storage/innobase/handler/ha_innodb.cc:8595-8601` writes
  `PAGE_ROOT_AUTO_INC` for persistent autoincrement tables after a successful
  explicit autoincrement update.
- `mariadb/storage/mylite/ha_mylite.cc:2058-2111` checks MyLite duplicate and
  FK update failures before publishing explicit autoincrement advancement.
- `mariadb/storage/mylite/ha_mylite.cc:2112-2134` appends the replacement row
  only after explicit autoincrement advancement succeeds.
- `packages/mylite-storage/src/storage.c:3039-3051` exposes the checkpoint
  marker used by statement rollback to preserve autoincrement pages appended
  after the statement checkpoint.
- `packages/mylite-storage/src/storage.c:3137-3168` collects autoincrement pages
  appended after a checkpoint only when the checkpoint is a transaction,
  savepoint, or has the explicit preservation marker.

## Scope

- Durable MyLite-routed first-key autoincrement tables requested as
  `ENGINE=InnoDB`.
- Ordered multi-row `UPDATE` where the first row successfully changes the
  autoincrement column to a high value.
- A later row in the same statement fails a MyLite foreign-key restriction,
  forcing statement rollback of the earlier visible row change.
- Close/reopen persistence for the preserved next value.

## Non-Goals

- Exhaustive duplicate-key, CHECK, trigger, view, generated-column,
  multi-table, grouped later-in-key, offset/increment, or integer-width update
  matrices.
- Failed update attempts that do not pass MyLite row checks; those remain
  covered by the failed-DML matrices and should not consume attempted values.
- Public insert-id API behavior.
- Size-profile reduction work.

## Compatibility Impact

This aligns MyLite with MariaDB/InnoDB's non-transactional persistent
autoincrement behavior for a prior-success failed update shape. The row changes
from the failed statement are rolled back, but a successful explicit
autoincrement advancement published before the later failure remains durable
and is used by the next generated insert.

The claim is still representative. Broader update matrices remain planned for
other error types, grouped autoincrement, offsets, triggers, views, and
multi-table updates.

## Design

Mark the active statement checkpoint for autoincrement preservation after a
durable MyLite update row successfully passes duplicate/FK checks and calls
`mylite_advance_auto_increment_from_row()`. This mirrors the generated insert
path's preservation marker and lets statement rollback restore row/index
visibility while keeping any autoincrement pages appended by prior successful
row updates in the same statement.

The marker is scoped to durable tables with an autoincrement field. Failed
update attempts still run duplicate/FK checks before advancement, so they do
not publish or preserve attempted high values.

## File Lifecycle

No file-format change is required. The behavior uses existing autoincrement
pages in the primary `.mylite` file and the existing statement checkpoint
rollback path.

## Embedded Lifecycle And API

No `libmylite` API change is required. The behavior is observable through
direct SQL execution.

## Storage-Engine Routing

The coverage uses requested `ENGINE=InnoDB`, which routes to MyLite storage in
the default embedded profile. Omitted/default, MyISAM, and Aria first-key
tables use the same durable MyLite path but are not repeated here.

## Build, Size, And Dependencies

No dependency, license, or intended size-profile change is introduced.

## Test Plan

- Keep the existing failed-DML matrix for a failure before any successful
  explicit autoincrement update publication.
- Add storage-engine smoke coverage for the ordered prior-success shape:
  update one parent row to a high autoincrement value, then fail a later parent
  row against an FK-restricted child.
- Verify row images and child references roll back, no high-id rows remain
  visible, the next generated insert resumes after the prior successful high
  value, and close/reopen preserves that next value.
- Run the rebuilt storage-smoke MariaDB archive, focused storage-engine test,
  statement-rollback and transaction compatibility harness groups, shell syntax
  checks, `git diff --check`, and the dev, embedded-dev, and storage-smoke
  presets.

## Acceptance Criteria

- Failed update attempts before publication still leave attempted high values
  unused.
- Prior successful explicit high-value update rows advance the durable next
  value even if a later row makes the statement fail.
- Statement rollback restores row/index visibility while preserving the
  published autoincrement advancement.
- Close/reopen resumes from the preserved next value.
- Roadmap and compatibility docs remove prior-success failed update from the
  immediate autoincrement gap list without claiming exhaustive update matrices.

## Risks And Open Questions

- The marker is conservative for durable updates on tables with an
  autoincrement field; rollback only preserves autoincrement pages appended
  after the checkpoint, so ordinary updates without such pages do not change
  state.
- Other error paths may reveal additional ordering differences and remain
  planned as broader update matrices.

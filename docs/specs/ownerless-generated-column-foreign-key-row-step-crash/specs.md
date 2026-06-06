# Ownerless Generated-Column Foreign-Key Row-Step Crash

## Problem

Ownerless generated-column foreign-key action crash coverage kills parent
delete writers immediately before `row_update_cascade_for_mysql()` and after a
successful child-side cascade returns. The remaining bounded native boundary is
inside InnoDB's row-update executor: a writer can die after the FK cascade node
has entered `row_upd_step()` but before the child-table update/delete call is
applied.

MyLite must prove no-live ownerless recovery still clears the interrupted
writer's volatile state, preserves the original generated-column parent/child
rows, and leaves the same parent delete retryable.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/row/row0ins.cc`
  `row_ins_foreign_check_on_constraint()` prepares the cascade node, stores the
  parent cursor position, commits the mini-transaction, sets
  `UPD_NODE_UPDATE_CLUSTERED`, and calls `row_update_cascade_for_mysql()`.
- `mariadb/storage/innobase/row/row0mysql.cc`
  `row_update_cascade_for_mysql()` increments `thr->fk_cascade_depth`, runs
  `row_upd_step()` for the child-table action, and then reports the
  transaction error state back to the parent operation.
- `mariadb/storage/innobase/row/row0upd.cc` `row_upd_step()` acquires the
  update-node table IX lock when needed, transitions to
  `UPD_NODE_UPDATE_CLUSTERED`, and calls `row_upd()` to apply the child-table
  update/delete.
- Existing hook coverage uses unsafe `foreign-key-action-before-execute` and
  `foreign-key-action-after-execute` fault points around
  `row_update_cascade_for_mysql()`. This slice adds an unsafe
  `foreign-key-action-row-step-before-update` fault point inside
  `row_upd_step()` while `trx->fk_cascade_depth > 0`.

## Scope And Non-Goals

In scope:

- Hook-build crash injection inside `row_upd_step()` before `row_upd()` applies
  a child-side generated-column FK cascade.
- The existing MariaDB-supported generated-column FK shapes:
  - stored generated child column `parent_key = raw_parent + 100` referencing a
    regular parent primary key,
  - stored generated referenced column `parent_key = base + 200` referenced by
    a regular child column.
- Live-peer cleanup refusal, no-live recovery of the original parent/child
  rows, retry of the same deletes, and ownerless/native reopen before and after
  forced `.shm` rebuild.

Out of scope:

- Crashes after `row_upd()` has modified a child row.
- Exhaustive row-level crash fuzzing across every row in a multi-row cascade.
- MariaDB-rejected generated-column FK action clauses.
- External MariaDB/RQG generated-column FK stress.

## Design

- Add a dormant unsafe fault point in `row_upd_step()`:
  `foreign-key-action-row-step-before-update`.
- Gate it with both the existing `mylite_ownerless_innodb_test_faults_enabled`
  fast check and `thr->fk_cascade_depth > 0` so ordinary updates and normal
  builds do not enter the fault path.
- Add a hook-only
  `generated-column-foreign-key-action-row-step-crash` selector that reuses the
  generated-column FK action crash helper with two row-step fault writers.
- The selector kills one writer while deleting a generated-child parent row and
  another while deleting a generated-referenced parent row. After each kill, a
  live ownerless peer must keep cleanup busy until no-live recovery can rebuild
  volatile ownerless state.
- After recovery, retry the same deletes, insert valid rows beyond the cascade
  boundary, and verify final generated values, FK metadata, and row state
  through ownerless and ordinary native reopen before and after forced shared
  memory rebuild.

## Compatibility Impact

No SQL surface is added. The slice strengthens crash-recovery evidence for
MariaDB-supported generated-column `ON DELETE CASCADE` paths under ownerless
mode. It does not claim complete generated-column FK crash coverage.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The test verifies the interrupted
writer leaves all durable state inside the MyLite database directory and that
no-live ownerless recovery can clean volatile coordination before retry.

## Native Storage Impact

No native storage format change. The new code is a hook-build test fault in an
upstream-derived InnoDB file and remains dormant unless unsafe ownerless test
faults are enabled and the configured fault name matches.

## Public API, Build, Size, And Dependencies

No public API, dependency, license, or production binary-size change is
intended. Normal builds only add a small gated branch in an existing unsafe
test-fault path.

## Test Plan

- Rebuild the MariaDB embedded archive after the `row0upd.cc` change.
- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused hook selectors:
  - `generated-column-foreign-key-action-row-step-crash`
  - `generated-column-foreign-key-action-crash`
  - `generated-column-foreign-key-action-after-crash`
- Run adjacent non-hook selectors:
  - `generated-column-foreign-key`
  - `generated-column-foreign-key-policy`
  - `foreign-key-actions`
- Run the relevant hook ownerless CTest shard, embedded ownerless CTest shard,
  ownerless stress, `format-check`, and `git diff --check`.

## Acceptance Criteria

- A killed generated-child parent delete inside `row_upd_step()` leaves the
  original parent row and generated child row visible after no-live recovery.
- Retrying that delete cascades the generated child row.
- A killed generated-referenced parent delete inside `row_upd_step()` leaves
  the generated parent row and ordinary child row visible after no-live
  recovery.
- Retrying that delete cascades the ordinary child row.
- Live-peer cleanup remains busy until the surviving ownerless peer closes.
- Final generated values, FK metadata, and cascaded row state survive
  ownerless/native reopen before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This fault fires before `row_upd()` applies the child update/delete, so it is
  deeper than the `row_update_cascade_for_mysql()` boundary but still not a
  partial-child-row modification crash.
- Full row-level crash fuzzing, external MariaDB/RQG generated-column FK
  stress, and broad randomized FK graph crash coverage remain planned.

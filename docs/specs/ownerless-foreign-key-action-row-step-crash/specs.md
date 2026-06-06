# Ownerless Foreign-Key Action Row-Step Crash

## Problem

Ownerless ordinary foreign-key action crash coverage kills parent
update/delete writers before `row_update_cascade_for_mysql()` and after a
successful child-side action returns. A deeper bounded native boundary is
inside InnoDB's row-update executor: a writer can die after the cascade node
enters `row_upd_step()` but before `row_upd()` applies the child-table
update/delete.

MyLite must prove no-live ownerless recovery clears the interrupted writer's
volatile state, preserves the original ordinary parent/child rows, and leaves
the same parent update/delete retryable.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/row/row0ins.cc`
  `row_ins_foreign_check_on_constraint()` prepares the cascade node, stores the
  parent cursor position, commits the mini-transaction, sets
  `UPD_NODE_UPDATE_CLUSTERED`, and calls `row_update_cascade_for_mysql()`.
- `mariadb/storage/innobase/row/row0mysql.cc`
  `row_update_cascade_for_mysql()` increments `thr->fk_cascade_depth`, runs
  `row_upd_step()` for the child-table action, and reports the transaction
  error state back to the parent operation.
- `mariadb/storage/innobase/row/row0upd.cc` `row_upd_step()` acquires the
  update-node table IX lock when needed, transitions to
  `UPD_NODE_UPDATE_CLUSTERED`, and calls `row_upd()` to apply the child-table
  update/delete.
- The existing unsafe ownerless test-fault point
  `foreign-key-action-row-step-before-update` fires inside `row_upd_step()`
  while `thr->fk_cascade_depth > 0`, before `row_upd()` applies the child
  update/delete.

## Scope And Non-Goals

In scope:

- Hook-build crash injection inside `row_upd_step()` before `row_upd()` applies
  an ordinary child-side FK action.
- The existing ordinary parent-key `ON UPDATE CASCADE` case.
- The existing ordinary parent-delete `ON DELETE CASCADE` plus
  `ON DELETE SET NULL` case.
- Live-peer cleanup refusal, no-live recovery of the original parent/child
  rows, retry of the same actions, and ownerless/native reopen before and after
  forced `.shm` rebuild.

Out of scope:

- Crashes after `row_upd()` has modified a child row.
- Exhaustive row-level crash fuzzing across every row in a multi-row cascade.
- Generated-column FK row-step crashes, covered separately by
  `ownerless-generated-column-foreign-key-row-step-crash`.
- Deep graph, cyclic, composite, and randomized FK crash matrices.
- SQL-level table-lock fault injection.
- External MariaDB/RQG randomized crash testing.

## Design

- Reuse the dormant unsafe fault point
  `foreign-key-action-row-step-before-update` in `row_upd_step()`.
- Add a hook-only `foreign-key-action-row-step-crash` selector to
  `mylite_ownerless_cross_process_sql_test`.
- Reuse the ordinary FK action crash helper with two new fault writers:
  one parent-key update and one parent delete, both waiting for the row-step
  fault name.
- After each killed writer, the selector verifies live-peer cleanup stays busy
  until the surviving ownerless peer closes, then no-live recovery restores the
  original parent/child rows and the same action can be retried.
- Final state is checked through ownerless and ordinary native reopen before
  and after forced shared-memory rebuild.

## Compatibility Impact

No SQL surface is added. The slice strengthens crash-recovery evidence for
MariaDB-supported ordinary `ON UPDATE CASCADE`, `ON DELETE CASCADE`, and
`ON DELETE SET NULL` paths under ownerless mode. It does not claim complete
ordinary FK crash coverage after child rows have already been modified.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The test verifies the interrupted
writer leaves durable state inside the MyLite database directory and that
no-live ownerless recovery can clean volatile coordination before retry.

## Native Storage Impact

No native storage format change. The new selector relies on native InnoDB
rollback for the interrupted parent and child changes, plus MyLite no-live
ownerless recovery to clear volatile transaction, lock, and page-version state.

## Public API, Build, Size, And Dependencies

No public API, dependency, license, or production binary-size change is
intended. The production hook was added by the generated-column row-step slice
and remains dormant unless unsafe ownerless test faults are enabled.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused hook selectors:
  - `foreign-key-action-row-step-crash`
  - `foreign-key-action-crash`
  - `foreign-key-action-after-crash`
- Run adjacent non-hook selector:
  - `foreign-key-actions`
- Run the relevant hook ownerless CTest shard, embedded ownerless CTest shard,
  ownerless stress, `format-check`, and `git diff --check`.

## Acceptance Criteria

- A killed parent-key update inside `row_upd_step()` leaves the original
  parent row and child parent ids visible after no-live recovery.
- Retrying that update cascades the child rows.
- A killed parent delete inside `row_upd_step()` leaves the original parent,
  cascade child, and nullable child rows visible after no-live recovery.
- Retrying that delete deletes cascade children and sets nullable child keys to
  `NULL`.
- Live-peer cleanup remains busy until the surviving ownerless peer closes.
- Final ordinary FK action state survives ownerless/native reopen before and
  after forced `.shm` rebuild.

## Risks And Follow-Up

- This fault fires before `row_upd()` applies the child update/delete, so it is
  deeper than the `row_update_cascade_for_mysql()` boundary but still not a
  partial-child-row modification crash.
- Full row-level crash fuzzing, external MariaDB/RQG FK graph stress, and
  randomized ordinary FK crash coverage remain planned.

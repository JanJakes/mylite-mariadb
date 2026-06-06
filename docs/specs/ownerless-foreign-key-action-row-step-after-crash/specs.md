# Ownerless Foreign-Key Action Row-Step After-Crash

## Problem

Existing ownerless foreign-key action crash coverage kills writers before
InnoDB enters `row_update_cascade_for_mysql()`, after a successful child-side
referential action returns, and inside `row_upd_step()` before `row_upd()`
applies a child-table update/delete. The remaining deterministic boundary is a
writer dying after one child-table row operation has succeeded inside
`row_upd_step()`, but before the parent statement commits.

MyLite must prove no-live ownerless recovery rolls back that partially applied
child-side action, clears volatile ownerless state, and leaves the same parent
action retryable.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/row/row0ins.cc`
  `row_ins_foreign_check_on_constraint()` prepares the cascade node and calls
  `row_update_cascade_for_mysql()` for the child-side referential action.
- `mariadb/storage/innobase/row/row0mysql.cc`
  `row_update_cascade_for_mysql()` increments `thr->fk_cascade_depth`, runs
  `row_upd_step()` for the child table, and returns the native error state to
  the parent operation before the parent statement commits.
- `mariadb/storage/innobase/row/row0upd.cc` `row_upd_step()` calls
  `row_upd(node, thr)`, stores the result in `trx->error_state`, returns on
  error, and otherwise advances the update node to fetch the next row or return
  to the parent.
- The existing unsafe fault point
  `foreign-key-action-row-step-before-update` fires before `row_upd()`. This
  slice adds `foreign-key-action-row-step-after-update` after a successful
  `row_upd()` while `thr->fk_cascade_depth > 0`.

## Scope And Non-Goals

In scope:

- Hook-build crash injection after a successful child-side `row_upd()` call in
  an FK cascade.
- Ordinary `ON UPDATE CASCADE`, `ON DELETE CASCADE`, and `ON DELETE SET NULL`
  parent actions with multiple affected child rows.
- MariaDB-supported generated-column `ON DELETE CASCADE` shapes:
  - stored generated child column `parent_key = raw_parent + 100` referencing a
    regular parent primary key,
  - stored generated referenced column `parent_key = base + 200` referenced by
    a regular child column.
- Live-peer cleanup refusal, no-live rollback of parent/child state, retry of
  the same actions, and ownerless/native reopen before and after forced `.shm`
  rebuild.

Out of scope:

- Exhaustive crash fuzzing after every child row in every cascade variant.
- Deep graph, cyclic, composite, and randomized FK crash matrices.
- SQL-level table-lock fault injection.
- External MariaDB/RQG randomized crash testing.

## Design

- Add a dormant unsafe fault point in `row_upd_step()` named
  `foreign-key-action-row-step-after-update`.
- Gate the hook with `thr->fk_cascade_depth > 0` and
  `mylite_ownerless_innodb_test_faults_enabled_fast()` so normal builds avoid
  fault work unless ownerless test faults are enabled.
- Extend the ordinary FK action crash fixture to seed two child rows for each
  affected parent key in both CASCADE and SET NULL child tables. The existing
  pre-action, post-action, and before-row-step selectors continue to use the
  same fixture.
- Add hook-only selectors:
  - `foreign-key-action-row-step-after-crash`
  - `generated-column-foreign-key-action-row-step-after-crash`
- Reuse the generated-column FK crash fixture with two affected child rows for
  each generated-column cascade shape, then insert replacement rows with new
  primary keys after retry so the final aggregate oracle remains unchanged.

## Compatibility Impact

No SQL surface is added. This narrows ownerless crash-recovery evidence for
MariaDB-supported FK referential actions by proving a killed ownerless writer
after a child-row operation does not expose or preserve partial child-side
state. It does not claim exhaustive FK crash fuzzing.

## Directory And Lifecycle Impact

No directory layout change. The tests verify the interrupted writer leaves
durable state inside the MyLite database directory, live-peer cleanup remains
busy while a peer is active, and no-live ownerless recovery can clear volatile
state before retry.

## Native Storage Impact

No native storage format change. The slice relies on native InnoDB transaction
rollback for the interrupted parent and child modifications, plus MyLite
no-live ownerless recovery to discard volatile transaction, lock, redo, and
page-version state.

## Public API, Build, Size, And Dependencies

No public API, dependency, license, or production binary-size change is
intended. The only production source change is a small guarded unsafe-test
fault branch in an upstream-derived InnoDB row executor.

## Test Plan

- Rebuild the MariaDB embedded archive after the `row0upd.cc` change when the
  preset requires it.
- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused hook selectors:
  - `foreign-key-action-row-step-after-crash`
  - `generated-column-foreign-key-action-row-step-after-crash`
  - `foreign-key-action-row-step-crash`
  - `generated-column-foreign-key-action-row-step-crash`
- Run adjacent non-hook selectors if the build/test cycle shows fixture
  regressions:
  - `foreign-key-actions`
  - `generated-column-foreign-key`
  - `generated-column-foreign-key-policy`
- Run the relevant hook ownerless CTest subset, `format-check`, and diff
  checks before commit.

## Acceptance Criteria

- A killed ordinary parent-key update after one child-row `row_upd()` leaves
  the original parent row and all original child parent ids visible after
  no-live recovery.
- Retrying that update cascades all affected child rows.
- A killed ordinary parent delete after one child-row `row_upd()` leaves the
  original parent, CASCADE child rows, and SET NULL child rows visible after
  no-live recovery.
- Retrying that delete removes CASCADE children and sets nullable child keys to
  `NULL`.
- Killed generated-column parent deletes after one child-row `row_upd()` leave
  their original generated or referenced child rows visible after no-live
  recovery, then retry successfully.
- Final ordinary and generated-column FK states survive ownerless/native reopen
  before and after forced `.shm` rebuild.

## Risks And Follow-Up

- The hook proves the first deterministic partial child-side operation in a
  multi-row cascade. Randomized fault selection across later child rows and
  broader FK graph shapes remains planned.
- Long-running external MariaDB/RQG FK graph execution remains
  environment-owned follow-up work.

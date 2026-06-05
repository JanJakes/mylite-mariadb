# Ownerless Foreign-Key Action After-Crash

## Problem

Ownerless FK action crash coverage currently kills writers immediately before
InnoDB executes child-side referential actions. That proves the prepared native
cascade state can be abandoned and retried, but it does not prove the next
native boundary: a writer dying after InnoDB has executed a child-side action
but before the parent statement commits. No-live ownerless recovery must leave
the uncommitted parent and child changes rolled back, clear volatile ownerless
state, and let the same action retry.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/row/row0ins.cc`
  `row_ins_foreign_check_on_constraint()` prepares the cascade node, stores
  cursor positions, commits the current mini-transaction, sets
  `UPD_NODE_UPDATE_CLUSTERED`, and calls
  `row_update_cascade_for_mysql()` for the child-side referential action.
- `mariadb/storage/innobase/row/row0mysql.cc`
  `row_update_cascade_for_mysql()` executes `row_upd_step()` for the child
  table, waits/retries on `DB_LOCK_WAIT`, resets `fk_cascade_depth`, updates
  child-table statistics on `DB_SUCCESS`, and returns the native error state to
  the parent operation.
- After `row_update_cascade_for_mysql()` returns, `row0ins.cc` restarts the
  mini-transaction and restores the parent cursor position before returning to
  the parent row update/delete path.
- MyLite already has a dormant
  `foreign-key-action-before-execute` ownerless test fault in this same
  source location, so adding a second hook after successful child action
  execution keeps the upstream delta narrow and test-only.

## Scope And Non-Goals

In scope:

- Hook-build crash injection after a successful `row_update_cascade_for_mysql()`
  return and before the parent cursor restore.
- Ordinary InnoDB FK `ON UPDATE CASCADE` parent-key update after child action
  execution.
- Ordinary InnoDB FK `ON DELETE CASCADE` plus `ON DELETE SET NULL` parent
  delete after child action execution.
- Live-peer cleanup refusal, no-live rollback of uncommitted parent/child
  state, retry of the same actions, and ownerless/native reopen before and
  after forced `.shm` rebuild.

Out of scope:

- Generated-column FK post-action crashes.
- Deep cascade chains, cyclic FK graphs, and composite FK post-action matrices.
- SQL-level table-lock fault injection.
- External MariaDB/RQG randomized crash testing.

## Design

- Add a new unsafe InnoDB fault name,
  `foreign-key-action-after-execute`, at the `row0ins.cc` call site after
  `row_update_cascade_for_mysql()` returns `DB_SUCCESS`.
- Keep the hook behind the existing
  `mylite_ownerless_innodb_test_faults_enabled_fast()` guard, so normal builds
  and non-hook tests pay only the existing disabled branch pattern.
- Add focused hook-only fault writers for parent update and parent delete that
  execute the same SQL as the existing pre-action crash selector but wait on
  `foreign-key-action-after-execute`.
- Extend ordinary FK action crash coverage with a new selector so pre-action
  and post-action crash evidence remain distinguishable in logs and docs.

## Compatibility Impact

This narrows ownerless crash-recovery evidence for ordinary InnoDB referential
actions. It does not change accepted SQL, public C APIs, storage formats, or
runtime directory layout, and it does not claim generated-column or graph-wide
post-action crash coverage.

## Directory And Lifecycle Impact

No new durable files or directory layout changes. The selector verifies that a
dead ownerless writer after native child-action execution is recovered inside
the MyLite-owned database directory lifecycle.

## Native Storage Impact

No storage format change. The slice relies on native InnoDB transaction
rollback for both parent and child modifications when the owning process dies
before commit, and on MyLite no-live ownerless recovery to discard volatile
ownerless transaction, lock, and page-version state before retry.

## Binary Size And Dependencies

No dependency or license changes. Production impact is a dormant unsafe-test
fault check in an existing MyLite-owned InnoDB hook region.

## Test Plan

- Rebuild the MariaDB embedded archive after the InnoDB row-layer edit.
- Configure and build `ownerless-test-hooks`.
- Run focused hook selectors:
  `foreign-key-action-after-crash` and `foreign-key-action-crash`.
- Run adjacent non-hook FK action coverage: `foreign-key-actions`.
- Run hook ownerless negative-proof CTest, embedded ownerless cross-process SQL
  CTest, ownerless stress, `format-check`, `tidy`, and diff checks before
  commit.

## Acceptance Criteria

- A killed parent-key update after child-side `ON UPDATE CASCADE` execution
  leaves the original parent id and child parent ids visible after no-live
  recovery.
- Retrying that parent-key update succeeds and cascades the child rows.
- A killed parent delete after child-side `ON DELETE CASCADE`/`SET NULL`
  execution leaves the original parent, cascade child, and nullable child rows
  visible after no-live recovery.
- Retrying that parent delete succeeds, deleting cascade children and setting
  nullable child keys to `NULL`.
- Live-peer cleanup remains busy until the surviving ownerless peer closes.
- Final state survives ownerless/native reopen before and after forced `.shm`
  rebuild.

## Risks And Follow-Up

- The hook fires after `row_update_cascade_for_mysql()` returns success, but
  before parent cursor restore and parent statement commit. It does not prove
  every intra-child-row partial-progress point inside `row_upd_step()`.
- Generated-column FK post-action crashes should be a separate slice after the
  ordinary FK post-action boundary is proven.
- Long-running external MariaDB/RQG FK graph execution remains
  environment-owned follow-up work.

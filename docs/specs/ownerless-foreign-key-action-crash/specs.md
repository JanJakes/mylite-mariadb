# Ownerless Foreign-Key Action Crash

## Problem

Ownerless foreign-key action coverage proves cross-process visibility for
representative `ON UPDATE CASCADE`, `ON DELETE CASCADE`, `ON DELETE SET NULL`,
and `ON DELETE RESTRICT` behavior. The remaining crash gap is narrower: if an
ownerless writer dies while InnoDB is about to execute a referential action,
the next no-live ownerless recovery must roll back the interrupted parent DML,
clean ownerless transaction and lock state, and leave the same action retryable.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/row/row0upd.cc`
  `row_upd_check_references_constraints()` detects parent-row updates and
  deletes that touch referenced index fields, opens any missing child table,
  and calls `row_ins_check_foreign_constraint()`.
- `mariadb/storage/innobase/row/row0ins.cc`
  `row_ins_check_foreign_constraint()` calls
  `row_ins_foreign_check_on_constraint()` when a matching child row exists and
  the foreign-key type has referential action flags.
- `row_ins_foreign_check_on_constraint()` rejects no-action paths with
  `DB_ROW_IS_REFERENCED`, prepares the cascade node, sets child row/table locks,
  builds the `SET NULL` or `CASCADE` update vector, stores cursor positions,
  commits the current mini-transaction, and then calls
  `row_update_cascade_for_mysql()` to execute the child-side action.
- MyLite ownerless cleanup already treats a crashed registered transaction as
  live-peer unsafe until no-live recovery can rebuild volatile state.

## Scope And Non-Goals

In scope:

- Hook-build crash injection immediately before `row_update_cascade_for_mysql()`.
- A crashed `ON UPDATE CASCADE` parent-key update before child action execution.
- A crashed `ON DELETE` parent delete before child `CASCADE`/`SET NULL` action
  execution.
- Live-peer cleanup refusal until the surviving ownerless peer closes.
- No-live recovery of the original parent/child state, successful retry of the
  same action, and ownerless/native reopen before and after forced `.shm`
  rebuild.

Out of scope:

- Crashes after a child action has modified child rows; ordinary FK coverage is
  handled by `ownerless-foreign-key-action-after-crash`, while generated-column
  and graph-scale variants remain separate work.
- Deep cascade chains, cyclic graphs, generated-column foreign keys, and
  composite foreign keys; those remain covered by their existing positive
  selectors and future crash matrices.
- SQL-level table-lock fault injection.
- External MariaDB/RQG randomized crash testing.

## Design

- Add a disabled-by-default InnoDB ownerless test-fault flag and helper in the
  existing MyLite InnoDB hook module.
- Enable that flag only from MyLite builds compiled with
  `MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS` when ownerless InnoDB hooks are
  installed. Normal embedded builds and non-ownerless opens leave the flag off.
- In `row_ins_foreign_check_on_constraint()`, call the test-fault helper after
  InnoDB stores the cascade cursor position and sets
  `UPD_NODE_UPDATE_CLUSTERED`, but before `row_update_cascade_for_mysql()`.
- Add the `foreign-key-action-crash` selector to
  `mylite_ownerless_cross_process_sql_test`.
- The selector creates parent, cascade child, and set-null child tables,
  crashes one writer during parent-key update, proves the original state is
  recovered and retryable, crashes another writer during parent delete, proves
  the original delete state is recovered and retryable, and validates the final
  successful action state through ownerless/native reopen before and after
  forced `.shm` rebuild.

## Compatibility Impact

This narrows the ownerless crash-recovery gap for InnoDB referential actions.
It does not claim complete foreign-key crash safety across every action phase;
ordinary post-action and row-step-before-update crash recovery are covered
separately, while generated-column and broader FK graph crash matrices remain
planned.

## Directory And Lifecycle Impact

No new durable files or directory layout changes. The hook test verifies that a
dead ownerless writer inside native InnoDB FK action execution leaves recovery
within the documented MyLite database directory lifecycle.

## Native Storage Impact

No storage format changes. The slice relies on native InnoDB rollback for the
interrupted parent DML and on MyLite ownerless no-live recovery to clear
volatile ownerless lock/transaction state before retry.

## Binary Size And Dependencies

No dependency or license changes. The production code impact is a small
disabled atomic branch in the InnoDB referential-action path plus the dormant
test-fault helper already housed in the MyLite InnoDB hook module.

## Test Plan

- Rebuild the MariaDB embedded archive after the InnoDB row-layer edit.
- Configure and build `ownerless-test-hooks`.
- Run `mylite_ownerless_cross_process_sql_test foreign-key-action-crash`.
- Run adjacent selectors:
  `foreign-key-actions`, `dictionary-foreign-key-crash`,
  `dictionary-foreign-key-drop-crash`, and `fk-graph-stress` as appropriate.
- Run the hook ownerless negative-proof CTest label, embedded ownerless
  cross-process SQL label, ownerless stress, `format-check`, `tidy`, and diff
  checks before commit.

## Acceptance Criteria

- A killed update writer paused before referential-action execution does not
  leave parent id `10` or cascaded child keys visible after no-live recovery.
- The same parent-key update succeeds after recovery and cascades to both child
  tables.
- A killed delete writer paused before referential-action execution does not
  delete the parent, cascade child, or set-null child state after no-live
  recovery.
- The same parent delete succeeds after recovery, deleting cascade children and
  setting nullable child keys to `NULL`.
- Live-peer cleanup remains busy while another ownerless process is still open.
- The final state survives ownerless/native reopen before and after forced
  `.shm` rebuild.

## Risks And Follow-Up

- The hook is before `row_update_cascade_for_mysql()`, so it proves rollback
  and retryability before child-side action execution. Ordinary post-action
  recovery after one child action has changed rows is covered by
  `ownerless-foreign-key-action-after-crash`; ordinary row-step entry before
  child update/delete application is covered by
  `ownerless-foreign-key-action-row-step-crash`. Generated-column and
  graph-scale post-action variants remain separate work.
- Long-running external MariaDB/RQG FK graph execution remains environment
  owned follow-up work.

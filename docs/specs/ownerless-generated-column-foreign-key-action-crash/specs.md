# Ownerless Generated-Column Foreign-Key Action Crash

## Problem

Ownerless generated-column foreign-key coverage proves two MariaDB-supported
stored generated-column FK shapes, and ordinary FK action crash coverage proves
rollback and retryability when a writer dies immediately before InnoDB executes
child-side referential actions. The remaining bounded intersection is generated
column FK action execution: if a parent delete dies after InnoDB prepares the
cascade node but before child-side `ON DELETE CASCADE` execution, no-live
ownerless recovery must restore the pre-action generated-column FK state and
leave the same delete retryable.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/mysql-test/suite/gcol/r/gcol_keys_innodb.result` records accepted
  stored generated child-column FK shapes for `ON UPDATE RESTRICT` and
  `ON DELETE CASCADE`, while generated-column `ON UPDATE CASCADE`,
  `ON UPDATE SET NULL`, and `ON DELETE SET NULL` variants are rejected with
  `ER_WRONG_FK_OPTION_FOR_GENERATED_COLUMN`.
- `mariadb/storage/innobase/dict/dict0crea.cc`
  `dict_foreigns_has_s_base_col()` and
  `mariadb/storage/innobase/handler/handler0alter.cc`
  `innobase_check_fk_stored()` preserve those generated-column action limits
  during create-time and ALTER-time FK definition.
- `mariadb/storage/innobase/row/row0ins.cc`
  `row_ins_foreign_check_on_constraint()` prepares cascade state, stores the
  cursor position, commits the current mini-transaction, sets
  `UPD_NODE_UPDATE_CLUSTERED`, and calls
  `row_update_cascade_for_mysql()` to execute child-side referential actions.
- The ownerless FK action crash hook now pauses immediately before
  `row_update_cascade_for_mysql()`, after native cascade state is prepared and
  before child rows are modified.

## Scope And Non-Goals

In scope:

- Hook-build crash injection immediately before child-side action execution for
  generated-column FK parent deletes.
- A stored generated child-column FK using
  `ON UPDATE RESTRICT ON DELETE CASCADE`.
- A stored generated referenced-column FK with a regular child column using
  `ON UPDATE RESTRICT ON DELETE CASCADE`.
- Live-peer cleanup refusal, no-live recovery of the original parent/child
  state, retry of the same deletes, and ownerless/native reopen before and
  after forced `.shm` rebuild.

Out of scope:

- Generated-column FK action clauses that MariaDB rejects.
- Virtual generated child-column FK action execution.
- Crashes after one or more child rows have already been modified.
- External MariaDB/RQG generated-column FK stress.

## Design

- Reuse the existing hook-only
  `foreign-key-action-before-execute` InnoDB fault point.
- Add `generated-column-foreign-key-action-crash` to
  `mylite_ownerless_cross_process_sql_test` behind
  `MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS`.
- The selector creates two table pairs:
  - `generated_child`: stored generated child `parent_key = raw_parent + 100`
    references a regular parent primary key.
  - `generated_ref`: stored generated parent `parent_key = base + 200` is
    unique and referenced by a regular child column.
- Crash one writer before deleting a generated-child parent row and another
  writer before deleting a generated-referenced parent row. In both cases, a
  live ownerless peer keeps cleanup busy until it closes.
- After no-live recovery, assert that the interrupted parent and child rows are
  still present, retry the same delete successfully, and validate the final
  cascaded state through ownerless/native reopen before and after forced
  `.shm` rebuild.

## Compatibility Impact

This narrows generated-column FK action-execution crash coverage to the
MariaDB-supported `ON DELETE CASCADE` shapes. It does not expand the accepted
SQL surface and does not claim post-child-action partial-progress recovery or
unsupported generated-column action clauses.

## Directory And Lifecycle Impact

No new durable files or directory layout changes. The selector verifies that a
dead writer inside native InnoDB generated-column FK action setup leaves
recovery inside the documented MyLite directory lifecycle.

## Native Storage Impact

No storage format change. The slice relies on native InnoDB rollback for the
interrupted parent delete and on MyLite no-live ownerless recovery to clear
volatile lock/transaction state before retry.

## Binary Size And Dependencies

No dependency or license changes. No new production code is required beyond the
existing dormant FK action fault hook.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `ownerless-test-hooks`.
- Run the focused selector:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test generated-column-foreign-key-action-crash`.
- Run adjacent hook selectors:
  `foreign-key-action-crash`,
  `dictionary-generated-column-foreign-key-crash`, and
  `dictionary-generated-column-foreign-key-drop-crash`.
- Run adjacent non-hook selectors:
  `generated-column-foreign-key`,
  `generated-column-foreign-key-policy`, and `foreign-key-actions`.
- Run hook ownerless negative-proof CTest, embedded ownerless cross-process SQL
  CTest, ownerless stress, `format-check`, `tidy`, and diff checks before
  commit.

## Acceptance Criteria

- A killed generated-child parent delete leaves the parent row and generated
  child row visible after no-live recovery.
- Retrying that delete cascades the generated child row.
- A killed generated-referenced parent delete leaves the parent row and
  ordinary child row visible after no-live recovery.
- Retrying that delete cascades the ordinary child row.
- Live-peer cleanup remains busy until the surviving ownerless peer closes.
- Final generated values, FK metadata, and cascaded row state survive
  ownerless/native reopen before and after forced `.shm` rebuild.

## Risks And Follow-Up

- The fault point is before `row_update_cascade_for_mysql()`, so partial
  child-action recovery after one child row changes remains separate work.
- MariaDB-rejected generated-column action clauses remain policy coverage, not
  action-crash coverage.
- Long-running external MariaDB/RQG generated-column FK stress remains
  environment-owned follow-up work.

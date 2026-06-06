# Ownerless Generated-Column Foreign-Key Action After-Crash

## Problem

Generated-column FK action crash coverage currently kills parent-delete
writers immediately before InnoDB executes child-side `ON DELETE CASCADE`.
The ordinary FK post-action slice adds a second fault point after successful
child-side action execution but before parent statement commit. The remaining
bounded generated-column gap is to prove the same post-action rollback and
retry behavior for MariaDB-supported stored generated-column FK shapes.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/mysql-test/suite/gcol/r/gcol_keys_innodb.result` records supported
  stored generated child-column and generated referenced-column FK shapes using
  `ON UPDATE RESTRICT` and `ON DELETE CASCADE`.
- `mariadb/storage/innobase/row/row0ins.cc`
  `row_ins_foreign_check_on_constraint()` prepares the cascade node, calls
  `row_update_cascade_for_mysql()`, and now has unsafe ownerless test-fault
  boundaries immediately before child-side action execution and after a
  successful child-side action returns.
- `mariadb/storage/innobase/row/row0mysql.cc`
  `row_update_cascade_for_mysql()` executes the child-table update/delete
  through `row_upd_step()` and returns the native error state to the parent
  operation before parent cursor restore and statement commit.

## Scope And Non-Goals

In scope:

- Hook-build crash injection after successful child-side `ON DELETE CASCADE`
  execution for generated-column FK parent deletes.
- Stored generated child-column FK:
  `parent_key = raw_parent + 100` references a regular parent primary key.
- Stored generated referenced-column FK:
  generated parent `parent_key = base + 200` is unique and referenced by a
  regular child column.
- Live-peer cleanup refusal, no-live rollback of uncommitted parent/child
  state, retry of the same deletes, and ownerless/native reopen before and
  after forced `.shm` rebuild.

Out of scope:

- Generated-column FK clauses that MariaDB rejects.
- Virtual generated child-column post-action crash coverage.
- Deep graph and cyclic generated-column FK crash matrices.
- External MariaDB/RQG generated-column FK stress.

## Design

- Reuse the unsafe `foreign-key-action-after-execute` InnoDB fault point.
- Add a hook-only `generated-column-foreign-key-action-after-crash` selector.
- Refactor the existing generated-column FK action crash selector into a shared
  helper parameterized by database name and fault writers, preserving the
  existing pre-action selector while adding post-action fault writers.
- The post-action selector uses the same two generated-column table pairs as
  the pre-action selector, crashes parent deletes after child action execution,
  verifies no-live recovery restores parent/child rows, retries the deletes,
  inserts valid rows past the cascade boundary, and validates final state
  through ownerless/native reopen before and after forced `.shm` rebuild.

## Compatibility Impact

This narrows generated-column FK crash-recovery evidence for MariaDB-supported
stored generated-column `ON DELETE CASCADE` shapes. It does not expand
accepted generated-column FK syntax and does not claim graph-wide randomized
crash coverage.

## Directory And Lifecycle Impact

No new durable files or directory layout changes. The selector verifies that
generated-column FK parent deletes killed after native child action execution
recover inside the documented MyLite database directory lifecycle.

## Native Storage Impact

No storage format change. The slice relies on native InnoDB transaction
rollback for uncommitted generated-column parent/child modifications and on
MyLite no-live ownerless recovery to discard volatile ownerless state before
retry.

## Binary Size And Dependencies

No dependency or license changes. No new production hook is required beyond
the existing unsafe post-action FK fault point.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `ownerless-test-hooks`.
- Run focused hook selectors:
  `generated-column-foreign-key-action-after-crash`,
  `generated-column-foreign-key-action-crash`, and
  `foreign-key-action-after-crash`.
- Run adjacent non-hook selectors:
  `generated-column-foreign-key`,
  `generated-column-foreign-key-policy`, and `foreign-key-actions`.
- Run hook ownerless negative-proof CTest, embedded ownerless cross-process SQL
  CTest, ownerless stress, `format-check`, `tidy`, and diff checks before
  commit.

## Acceptance Criteria

- A killed generated-child parent delete after child-side cascade execution
  leaves the parent row and generated child row visible after no-live recovery.
- Retrying that delete cascades the generated child row.
- A killed generated-referenced parent delete after child-side cascade
  execution leaves the parent row and ordinary child row visible after no-live
  recovery.
- Retrying that delete cascades the ordinary child row.
- Live-peer cleanup remains busy until the surviving ownerless peer closes.
- Final generated values, FK metadata, and cascaded row state survive
  ownerless/native reopen before and after forced `.shm` rebuild.

## Risks And Follow-Up

- The hook fires after one successful child-side referential action returns.
  The row-step slice covers entry into `row_upd_step()` before child-table
  update/delete application, but partial child-row modification crash fuzzing
  remains separate work.
- Long-running external MariaDB/RQG generated-column FK stress remains
  environment-owned follow-up work.

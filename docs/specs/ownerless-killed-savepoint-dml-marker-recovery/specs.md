# Ownerless Killed Savepoint DML Marker Recovery

## Problem

Ownerless explicit transactions now discard checkpointed DML file-operation
evidence when `ROLLBACK TO SAVEPOINT` leaves no earlier local write in the
transaction. The previous slice proved that behavior for a live handle that
later closes normally.

The remaining killed-session matrix needs the same proof when the process
commits after the savepoint rollback and then exits before `mylite_close()`.
That boundary matters because committed DML markers are durable coordination
evidence; a process death after `COMMIT` must not leave a DML marker for work
that MariaDB's savepoint rollback removed before the transaction outcome.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` maps `SAVEPOINT ident`,
  `ROLLBACK [WORK] TO [SAVEPOINT] ident`, and
  `RELEASE SAVEPOINT ident` to dedicated SQL commands.
- `mariadb/sql/transaction.cc:trans_rollback_to_savepoint()` calls
  `ha_rollback_to_savepoint()`, makes the target savepoint the transaction
  savepoint head, and deletes savepoints created after it.
- `mariadb/storage/innobase/handler/ha_innodb.cc:innobase_savepoint()` stores
  an InnoDB transaction undo number, and
  `innobase_rollback_to_savepoint()` rolls row state back to that undo number.
- `packages/libmylite/src/database.cc` publishes DML-specific native file-op
  checkpoint markers only after successful autocommit writes or explicit
  transaction `COMMIT` where MyLite still has local-write evidence.

## Design

Add focused Linux SQL coverage for a child process that:

- opens the database in ownerless read/write mode,
- starts an explicit transaction,
- creates a savepoint,
- updates a file-per-table InnoDB row after a native checkpoint,
- proves the process-local ownerless InnoDB file-op redo latch was set,
- rolls back to the savepoint and proves the latch was discarded,
- commits the transaction with both native file-op markers still clear,
- exits with `_exit(0)` before `mylite_close()`.

The parent waits until the writer is a zombie, verifies both file-op markers
remain clear before reaping, then opens the database ownerless, requires
recovery close to checkpoint any non-marker coordination WAL, forces `.shm`
rebuild, and opens it ordinary-native to prove the pre-savepoint row survives
and the table remains writable.

No product code, file formats, public APIs, or MariaDB source changes are
expected for this slice unless the focused test exposes a real bug.

## Compatibility Impact

SQL behavior remains MariaDB-owned. This is recovery evidence for MyLite's
ownerless checkpoint classification after a supported MariaDB savepoint
rollback and killed embedded process boundary.

## Directory And Lifecycle Impact

The test asserts no durable dictionary or DML file-op marker survives the killed
process after the savepoint-rolled-back commit. The database directory must
reopen through ownerless and ordinary native paths without manual cleanup, and
the recovering close must checkpoint retained non-marker WAL.

## Native Storage Impact

Native InnoDB undo remains the source of truth for the rolled-back row image.
The child process exit before close tests MyLite's ownerless coordination state,
not an alternate storage-engine recovery path.

## Binary Size And Dependencies

The slice adds one focused test helper, one selector, and one CTest entry. No
dependency or embedded MariaDB profile change is expected.

## Test Plan

- Add `native-killed-savepoint-dml-file-op-marker-recovery`.
- Register the selector in the ownerless SQL weighted harness and as a focused
  production CTest for visible timing.
- Run the new selector, adjacent killed DML marker selectors, the live
  savepoint discard selector, relevant ownerless transaction stress smoke,
  `format-check-prod`, and `git diff --check`.

## Acceptance Criteria

- A killed writer that committed after `ROLLBACK TO SAVEPOINT` leaves both
  native file-op markers clear while it is still a zombie.
- The next ownerless recovery close checkpoints any retained non-marker WAL.
- The next ownerless opener sees the pre-savepoint row and closes cleanly.
- Forced `.shm` rebuild and ordinary native reopen preserve the same row and
  allow a follow-up write.
- Existing killed committed/uncommitted DML marker and live savepoint marker
  selectors continue to pass.

## Risks

- This is a focused killed-session proof for the no-earlier-write savepoint
  boundary after successful savepoint rollback and commit. The
  killed-before-savepoint-rollback slice covers process death before rollback
  or commit, and the savepoint-rollback-before-state hook slice covers the
  MyLite-owned boundary after native rollback succeeds but before ownerless
  savepoint state is updated. Killed processes inside native rollback internals
  or amid concurrent writers remain separate matrices.

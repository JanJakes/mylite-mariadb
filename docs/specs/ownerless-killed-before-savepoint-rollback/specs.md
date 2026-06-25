# Ownerless Killed Before Savepoint Rollback

## Problem

Ownerless coverage already proves two adjacent transaction outcomes:

- a killed explicit transaction with no savepoint rolls back through native
  InnoDB recovery without exposing uncommitted DML, and
- a killed process that first rolls back to a savepoint, commits, and exits
  before `mylite_close()` leaves no durable DML marker for the rolled-back
  savepoint write.

The remaining gap between those outcomes is a process killed while the
explicit transaction still has an active savepoint stack and post-checkpoint
native DML evidence, before SQL has a chance to run `ROLLBACK TO SAVEPOINT` or
`COMMIT`. That boundary matters because no MyLite process-local savepoint
state survives `SIGKILL`; the next opener must rely on native InnoDB recovery
and ownerless coordination cleanup without turning either the pre-savepoint or
post-savepoint uncommitted row image into a committed ownerless page-version
boundary or DML checkpoint marker.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/transaction.cc:711` implements
  `trans_rollback_to_savepoint()`: it calls `ha_rollback_to_savepoint()`,
  resets the transaction savepoint head to the named savepoint, and releases
  metadata locks only when the storage engines allow it.
- `mariadb/sql/handler.cc:3382` implements
  `ha_rollback_to_savepoint()`: engines registered before the savepoint run
  their `savepoint_rollback` callback, while engines registered after the
  savepoint run transaction rollback and are removed from the transaction
  engine list.
- `mariadb/storage/innobase/handler/ha_innodb.cc:2920` stores the current
  InnoDB undo number in `innobase_savepoint()`, and
  `ha_innodb.cc:5026` rolls InnoDB state back to that undo number in
  `innobase_rollback_to_savepoint()`.
- `packages/libmylite/src/database.cc:15866` tracks savepoint-local ownerless
  write state only in the current process and discards the process-local native
  file-operation redo latch after a successful savepoint rollback when the
  savepoint proves there was no earlier local write.

## Design

Add focused Linux ownerless SQL coverage for a child process that:

- opens the database in ownerless read/write mode,
- starts an explicit transaction,
- updates one row before a savepoint,
- creates a savepoint,
- updates a second row after the savepoint,
- proves the process-local InnoDB file-operation redo latch has observed the
  DML path,
- signals the parent and then waits without rolling back or committing.

The parent kills the child with `SIGKILL`, verifies no durable native
file-operation marker was published by the uncommitted transaction, opens the
database in ownerless mode to force no-live recovery, and verifies both rows
still contain the pre-transaction values. The test then forces `.shm` rebuild
and opens through the ordinary native path to prove recovery did not depend on
volatile ownerless state and the table remains writable.

No product code, file format, public API, or MariaDB source change is expected
unless this focused test exposes a real bug.

## Compatibility Impact

SQL semantics remain MariaDB/InnoDB-owned. This slice adds recovery evidence
for MyLite's ownerless coordination state after a supported MariaDB explicit
transaction with savepoints is killed before any transaction-ending SQL.

## Directory And Lifecycle Impact

The database directory remains self-contained. The killed child exits without
`mylite_close()`, so the next ownerless opener must rebuild or clean ownerless
process state from durable coordination files and native InnoDB recovery. No
manual cleanup or external durable state is allowed.

## Native Storage Impact

Native InnoDB undo and crash recovery remain the source of truth for the
uncommitted pre-savepoint and post-savepoint row images. Ownerless cleanup must
not publish, checkpoint, or retain them as committed page-version evidence.

## Binary Size And Dependencies

The slice adds one focused test helper, one direct selector, one registered
ownerless SQL case, and docs. It does not add dependencies or change the
embedded MariaDB profile.

## Test Plan

- Add direct selector
  `native-killed-before-savepoint-rollback-dml-file-op-marker-recovery`.
- Register the test in the ownerless SQL weighted harness.
- Run the new selector in the production build.
- Run adjacent killed DML marker and savepoint marker selectors.
- Run the matching production ownerless SQL CTest shard, hook-build selector
  smoke, ownerless stress transaction smoke where practical,
  `format-check-prod`, and `git diff --check`.

## Acceptance Criteria

- The killed child has modified rows before and after a savepoint but has not
  committed, rolled back, or closed.
- Durable DDL and DML native file-operation markers remain clear before and
  after the child is killed.
- The next ownerless opener sees the original two-row aggregate and payload
  state after no-live recovery.
- Forced `.shm` rebuild plus ordinary native reopen see the same original row
  state and allow a follow-up native write.
- Adjacent killed committed, killed uncommitted, killed savepoint-rolled-back,
  live rollback, and live savepoint DML-marker selectors continue to pass.

## Risks

This closes a killed-before-savepoint-rollback boundary. The
`ownerless-savepoint-rollback-before-state-crash` slice separately covers the
MyLite-owned boundary after native savepoint rollback succeeds but before
ownerless process-local savepoint state is updated. Neither slice proves a kill
inside the InnoDB savepoint rollback routine, arbitrary concurrent-writer
savepoint schedules, broader native redo/checkpoint reconciliation, or
DDL/file-lifecycle recovery.

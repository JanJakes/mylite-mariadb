# Ownerless Same Page Savepoint Handoff

## Problem

Ownerless transaction coverage proves that a writer can roll back to a
savepoint and keep the transaction open while an independent-table peer
commits. The remaining transaction evidence gap calls out same-page and
same-table schedules where the first writer's rollback-to-savepoint rewrites
native pages, but page-write ownership must still prevent a peer from
publishing an overlapping same-page image until the first writer commits.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB rollback-to-savepoint runs native undo without ending the
  transaction.
- MariaDB `ha_innodb.cc:innobase_savepoint()` stores the transaction
  `undo_no` in engine-owned savepoint storage, and
  `innobase_rollback_to_savepoint()` calls `trx->rollback(savept)`.
- MyLite's InnoDB rollback hook clears current transaction-deferred page images
  after native rollback-to-savepoint because images captured after the target
  savepoint no longer describe the transaction outcome.
- MyLite now snapshots transaction-deferred page images at
  `innobase_savepoint()`, restores the target snapshot after successful
  `ROLLBACK TO SAVEPOINT`, releases it on `RELEASE SAVEPOINT`, and clears all
  snapshots during transaction cleanup.
- The ownerless page refresh path preserves pages that are dirty in the
  current transaction or have transaction-owned page images.
- Existing SQL coverage proves independent-table savepoint handoff. This slice
  forces the peer writer to wait on the same page after rollback, then verifies
  the pre-savepoint row image survives commit.

## Design

Add transaction-local page-image savepoint snapshots inside `trx_t`:

- on `SAVEPOINT`, store the current transaction-deferred page-image vector with
  the InnoDB savepoint token and undo number;
- on successful `ROLLBACK TO SAVEPOINT`, clear current post-savepoint images,
  restore the target snapshot, and discard snapshots newer than the target;
- on `RELEASE SAVEPOINT`, remove the released snapshot;
- on full transaction cleanup, clear all snapshots.

Keep full transaction rollback conservative: images captured before full undo
still do not prove rollback outcome and remain cleared.

The focused ownerless SQL test keeps three compact rows in one InnoDB clustered
page:

- writer A updates row 1, creates a savepoint, updates row 2, rolls back to
  the savepoint, and keeps the transaction open;
- writer B updates row 3 in the same table while writer A is still open;
- the test waits for the shared ownerless InnoDB/page-write waiting count to
  rise, proving B is serialized behind A's same-page ownership;
- after A commits, B completes, and the final state keeps A's pre-savepoint
  row 1 update, discards A's rolled-back row 2 update, and keeps B's row 3
  update.

## Compatibility Impact

No public API or SQL syntax changes. The implementation preserves
MariaDB-compatible transaction visibility and rollback semantics for a bounded
ownerless multi-process schedule where the committing transaction's surviving
pre-savepoint page image must remain publishable.

## Directory And Native Storage Impact

No new durable files or formats are added. Savepoint snapshots are
process-local transaction memory and are discarded at transaction cleanup. The
slice verifies ownerless page-write ownership, WAL retention, and native reopen
behavior.

## Test Plan

- Add selector `concurrent-savepoint-same-page-handoff`.
- Add CTest `libmylite.ownerless-concurrent-savepoint-same-page-handoff`.
- Rebuild the MariaDB embedded archive after the InnoDB transaction change.
- Verify the selector directly and with the adjacent savepoint/rollback CTest
  subset.
- Run production build guards, format check, and whitespace check.

## Acceptance Criteria

- Writer B is observed waiting while writer A holds the transaction open after
  `ROLLBACK TO SAVEPOINT`.
- The final ownerless, forced `.shm` rebuild, and ordinary native reopen state
  is row 1 committed by writer A, row 2 restored by the savepoint rollback,
  and row 3 committed by writer B.
- A repeated production run of the selector does not reproduce the prior
  `(10,20,31)` lost pre-savepoint row outcome.
- Native DML file-operation evidence drains after no-live close.

## Risks And Follow-Up

This is one deterministic same-page/same-table schedule and an eager copy of
page-image vectors at savepoint boundaries. Broader same-page writer matrices,
mid-native-rollback crash injection, copy-on-write snapshot optimization, and
external randomized transaction stress remain planned separately.

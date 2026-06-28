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
- MyLite's InnoDB rollback hook clears transaction-deferred page images and
  marks the transaction page-write publish path conservative after
  `ROLLBACK TO SAVEPOINT`.
- The ownerless page refresh path preserves pages that are dirty in the
  current transaction or have transaction-owned page images.
- Existing SQL coverage proves independent-table savepoint handoff, but does
  not force the peer writer to wait on the same page after the rollback.

## Design

Add a focused ownerless SQL test that keeps three compact rows in one InnoDB
clustered page:

- writer A updates row 1, creates a savepoint, updates row 2, rolls back to
  the savepoint, and keeps the transaction open;
- writer B updates row 3 in the same table while writer A is still open;
- the test waits for the shared ownerless InnoDB/page-write waiting count to
  rise, proving B is serialized behind A's same-page ownership;
- after A commits, B completes, and the final state keeps A's pre-savepoint
  row 1 update, discards A's rolled-back row 2 update, and keeps B's row 3
  update.

## Compatibility Impact

No public API or SQL behavior changes. The test asserts MariaDB-compatible
transaction visibility and rollback semantics for a bounded ownerless
multi-process schedule.

## Directory And Native Storage Impact

No new durable files or formats are added. The slice only adds verification
for existing ownerless page-write ownership, WAL retention, and native reopen
behavior.

## Test Plan

- Add selector `concurrent-savepoint-same-page-handoff`.
- Add CTest `libmylite.ownerless-concurrent-savepoint-same-page-handoff`.
- Verify the selector directly and with the adjacent savepoint/rollback CTest
  subset.
- Run production build guards, format check, and whitespace check.

## Acceptance Criteria

- Writer B is observed waiting while writer A holds the transaction open after
  `ROLLBACK TO SAVEPOINT`.
- The final ownerless, forced `.shm` rebuild, and ordinary native reopen state
  is row 1 committed by writer A, row 2 restored by the savepoint rollback,
  and row 3 committed by writer B.
- Native DML file-operation evidence drains after no-live close.

## Risks And Follow-Up

This is one deterministic same-page/same-table schedule. Broader same-page
writer matrices, mid-native-rollback crash injection, and external randomized
transaction stress remain planned separately.

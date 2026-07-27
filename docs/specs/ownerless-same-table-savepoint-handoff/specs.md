# Ownerless Same-Table Savepoint Handoff

## Problem Statement

Ownerless transaction coverage already proves independent-table savepoint
handoff and focused same-page serialization. The remaining transaction matrix
still includes same-table schedules where one writer rolls back post-savepoint
DML and keeps the transaction open while a peer commits unrelated DML in the
same table.

This slice covers a same-table large-row schedule. Writer A updates
one row, saves a point, updates a second row, rolls back to the savepoint, and
stays open. Writer B updates a fourth large row in the same table and must wait
only if it reaches a page still owned by writer A. In the deterministic
large-row layout, writer B reaches a different page and must complete while
writer A remains open. Final recovery must keep A's pre-savepoint row and B's
row, while discarding A's rolled-back row.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/transaction.cc:711-748` implements
  `trans_rollback_to_savepoint()`, resolves the SQL savepoint, delegates
  native rollback through `ha_rollback_to_savepoint()`, and then releases
  eligible metadata locks.
- `mariadb/sql/handler.cc:3382-3440` implements
  `ha_rollback_to_savepoint()` by calling each participating engine's
  savepoint rollback callback or full rollback for engines registered after
  the savepoint.
- `mariadb/storage/innobase/handler/ha_innodb.cc:2920-2933` records the
  InnoDB undo number for a savepoint, and
  `mariadb/storage/innobase/handler/ha_innodb.cc:5026-5069` rolls back to the
  saved undo number.
- `packages/libmylite/src/database.cc` updates process-local ownerless
  savepoint state only after successful SQL and uses native DML
  file-operation markers plus page-version WAL retention for cross-process
  handoff evidence.

## Scope And Non-Goals

In scope:

- A Linux ownerless SQL selector for same-table large-row savepoint handoff.
- Writer B completing while writer A keeps the transaction open.
- Native DML file-operation marker cleanup after both writers exit.
- Final ownerless reopen, forced `.shm` rebuild, and ordinary native reopen.

Out of scope:

- Crashes inside InnoDB native savepoint rollback internals.
- Same-row conflict schedules.
- Exhaustive row-packing or page-placement proofs.
- External MariaDB/RQG randomized transaction stress.

## Design

Add `concurrent-savepoint-same-table-handoff` to
`mylite_ownerless_cross_process_sql_test`.

The table uses four `VARBINARY(7000)` payload rows to broaden the same-table
coverage beyond the compact same-page test. Writer A:

1. starts a transaction,
2. updates row 1,
3. creates a savepoint,
4. updates row 2,
5. rolls back to the savepoint, and
6. waits before commit.

Writer B updates row 4 in the same table. The parent requires bounded peer
completion with no ownerless waiter leak, verifies that B is visible while A
remains open and A is not, releases A, and then verifies that final reopen paths
see row 1 plus row 4 but not row 2's rolled-back image, with no stale native
file-operation markers after no-live recovery.

## Compatibility Impact

No SQL feature or API behavior changes. This is compatibility evidence for
MariaDB savepoint rollback and explicit transaction semantics under ownerless
cross-process writes.

## Directory, Lifecycle, And Native Storage Impact

No directory layout, durable format, or native storage behavior changes. The
test exercises existing ownerless WAL retention, DML native file-operation
checkpoint markers, forced shared-memory rebuild, and ordinary native reopen.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive production change.
The slice adds one test selector, one CTest entry, and documentation.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run direct selector `concurrent-savepoint-same-table-handoff`.
- Run the adjacent savepoint handoff CTest subset.
- Build and run the selector against `php-embedded-prod`.
- Run transaction stress smoke, production-build guards, format check, and
  `git diff --check`.

## Acceptance Criteria

- Writer B completes while writer A remains open and no ownerless wait entry
  remains.
- While writer A remains open, a peer sees writer B's committed row but not
  writer A's uncommitted row.
- Native DML file-operation markers are clear after no-live recovery.
- Final ownerless reopen, forced `.shm` rebuild, and ordinary native reopen
  preserve row 1 from writer A, row 4 from writer B, and the original row 2.

## Risks And Follow-Up

- This is a bounded same-table large-row schedule, not proof of every possible
  row-packing or same-page combination.
- Native mid-rollback crash injection, same-row conflicts, broader same-table
  randomized schedules, and external MariaDB/RQG stress remain ownerless
  concurrency completion work.

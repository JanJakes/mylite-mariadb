# Ownerless Write-Skew Isolation

## Problem

Ownerless transaction coverage already proves row waits, gap-lock insert
blocking, serializable readers blocking peer writers, reverse-order deadlocks,
savepoint rollback, read-committed refresh, and repeatable snapshots. The
minimum ownerless test strategy still called out write-skew candidates as a
separate transaction-correctness shape: two transactions can each read a shared
predicate and then update disjoint rows. Under InnoDB `SERIALIZABLE`, those
plain reads should become shared locking reads, so both transactions must not
commit disjoint updates that violate the predicate.

## Source Findings

- `mariadb/storage/innobase/include/trx0trx.h` defines
  `TRX_ISO_SERIALIZABLE` as the level where plain `SELECT` statements are
  converted to `LOCK IN SHARE MODE` reads.
- `mariadb/storage/innobase/handler/ha_innodb.cc` keeps autocommit
  serializable consistent reads read-only, but otherwise treats
  non-autocommit serializable plain `SELECT` as a shared locking read.
- The ownerless InnoDB lock bridge mirrors shared and exclusive record/table
  conflicts through the directory-backed lock registry, and existing SQL
  coverage proves serializable shared reads can block a peer update.

## Design

Add a focused `write-skew` selector to
`mylite_ownerless_cross_process_sql_test` and include it in the normal
ownerless SQL run.

The test creates an InnoDB `ownerless_write_skew` table with two on-call rows.
Two ownerless child processes each:

1. set `innodb_lock_wait_timeout = 2`;
2. start a `SERIALIZABLE` transaction;
3. read `SUM(on_call)` and verify both rows are initially on call;
4. wait until the peer has the same serializable read lock; and
5. try to set its own row off call.

At least one child must report deadlock or lock-wait timeout rather than both
committing. The final state must keep at least one row on call, then survive
ownerless reopen, ordinary native exclusive reopen, forced `.shm` rebuild, and
native exclusive reopen after that rebuild.

## Scope And Non-Goals

In scope:

- A deterministic two-row write-skew candidate under `SERIALIZABLE`.
- Evidence that ownerless shared-read lock mirroring prevents both disjoint
  updates from committing.
- Reopen and forced shared-memory rebuild checks for the final state.

Out of scope:

- Claiming a complete isolation-level matrix.
- Randomized transaction or external MariaDB/RQG oracle stress.
- Predicate-lock edge cases beyond this bounded InnoDB row/predicate shape.

## Compatibility Impact

This preserves MariaDB/InnoDB `SERIALIZABLE` behavior for a common write-skew
candidate. It does not change SQL semantics; it adds evidence that ownerless
cross-process lock mirroring keeps inherited InnoDB isolation from degrading
when the two transactions live in different embedded processes.

## Directory And Lifecycle Impact

No new files or directory layout. The test exercises existing ownerless
process, transaction, lock, page-version WAL, checkpoint, and shared-memory
rebuild state.

## Native Storage Impact

No native storage format changes. The final committed state remains a native
InnoDB table inside the MyLite directory and is verified through both
ownerless and ordinary exclusive opens.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `write-skew` in `embedded-dev`.
- Build and run focused `write-skew` in `ownerless-test-hooks`.
- Run embedded ownerless SQL, hook ownerless SQL, ownerless stress,
  `format-check`, and diff checks.

## Acceptance Criteria

- Two serializable ownerless transactions both read the initial predicate.
- Both child transactions cannot commit their disjoint updates.
- The final state keeps at least one row on call.
- The final state survives ownerless/native reopen before and after forced
  `.shm` rebuild.
- Existing ownerless SQL, hook, and stress coverage remains green.

## Risks And Follow-Up

- This covers one deterministic write-skew candidate, not every predicate-lock
  or isolation-level matrix shape.
- External MariaDB/RQG long-running oracle stress remains planned.

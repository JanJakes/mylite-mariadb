# Ownerless Savepoint Prewrite Live-Peer Crash

## Problem Statement

Existing hook coverage kills an ownerless writer after native
`ROLLBACK TO SAVEPOINT` succeeds and before MyLite updates process-local
savepoint state, then recovers with no other ownerless process alive. A
remaining live-peer boundary is stricter: if another ownerless process still
has the database directory open, a new ownerless opener must not perform
no-live cleanup of the killed writer's stale transaction state. Once the live
peer exits, no-live ownerless recovery must still preserve MariaDB/InnoDB's
rolled-back result.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB documents InnoDB as the transactional storage engine responsible for
  ACID behavior, row-level locks, transaction logging, undo/MVCC, and crash
  recovery.
- `mariadb/sql/transaction.cc:711-748` implements
  `trans_rollback_to_savepoint()` and calls the storage-engine rollback path
  before releasing savepoint-bound metadata locks.
- `mariadb/sql/handler.cc:3382-3440` implements
  `ha_rollback_to_savepoint()` by iterating transaction participants and
  invoking each engine rollback callback.
- `mariadb/storage/innobase/handler/ha_innodb.cc:5026-5069` implements the
  InnoDB savepoint rollback callback.
- `packages/libmylite/src/database.cc:20407-20448`
  `update_ownerless_savepoint_state_after_successful_sql()` updates MyLite's
  savepoint write-state stack after MariaDB reports successful SQL execution.
  The unsafe hook `savepoint-rollback-before-state` pauses after native
  rollback and before MyLite refreshes that ownerless state.

## Design

Add a hook-build selector named
`savepoint-rollback-prewrite-live-peer-crash`.

The test:

1. Creates a two-row InnoDB table and checkpoints the initial state.
2. Opens a live ownerless peer and keeps it open.
3. Starts a writer that updates row 1, creates a savepoint, updates row 2,
   executes `ROLLBACK TO SAVEPOINT`, and pauses at the
   `savepoint-rollback-before-state` hook.
4. Kills the writer while the live peer remains open.
5. Requires a fresh ownerless read/write open to return `MYLITE_BUSY` while
   that peer remains live, proving cleanup is not attempted under a live-peer
   ambiguity.
6. Releases the peer and verifies no-live ownerless recovery preserves the
   original rows, keeps native file-operation markers clear, checkpoints
   ownerless WAL, survives forced `.shm` rebuild, and remains writable through
   an ordinary native reopen.

## Scope And Non-Goals

In scope:

- Linux unsafe ownerless test-hook coverage.
- One pre-savepoint write and one post-savepoint write in the same InnoDB
  table.
- A killed writer at MyLite's post-native/pre-state savepoint rollback
  boundary.
- Live-peer busy behavior before no-live recovery.
- Ownerless, forced-`.shm`, and ordinary native reopen oracles.

Out of scope:

- Fault injection inside InnoDB row-undo/savepoint-rollback internals.
- Arbitrary multi-writer savepoint schedules beyond the held live peer.
- DDL, foreign keys, triggers, generated columns, or routine side effects.
- SQL-level table-lock fault injection.

## Compatibility Impact

No SQL syntax, C API, or durable format behavior changes. The expected data
result follows MariaDB/InnoDB semantics: the killed uncommitted transaction is
rolled back completely, including the write before the savepoint. The
ownerless-specific compatibility claim is that a live peer prevents unsafe
cleanup of killed-writer state until no-live recovery can rebuild from native
state.

## Directory, Lifecycle, And Native Storage Impact

No directory layout changes. The test covers the single-directory ownerless
lifecycle by keeping all native InnoDB files, ownerless concurrency metadata,
and temporary runtime paths under the MyLite-owned directory/runtime root used
by the harness. The live peer keeps the directory in the cross-process
ownerless state, and the final no-live opener proves the recovery transition
back to ordinary ownerless/native accessibility.

## Public API, Build, Size, License

No public API, dependency, binary-size, or license changes. The selector is
registered only for the unsafe ownerless test-hook build.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run direct selector `savepoint-rollback-prewrite-live-peer-crash`.
- Run the new CTest
  `libmylite.ownerless-savepoint-rollback-prewrite-live-peer-crash`.
- Run adjacent savepoint and transaction rollback hook CTests.
- Run production adjacent transaction/savepoint selectors where applicable.
- Run ownerless primitive/live-reclaim checks, format check, CI production
  build audit, and `git diff --check`.

## Acceptance Criteria

- The writer pauses after native `ROLLBACK TO SAVEPOINT` and before MyLite
  updates ownerless savepoint state.
- A fresh ownerless read/write opener gets `MYLITE_BUSY` while the live peer is
  still open.
- After the peer exits, no-live ownerless recovery preserves the original row
  values and payloads.
- Native DML and DDL file-operation markers stay clear.
- Ownerless WAL checkpoints after recovery.
- Forced `.shm` rebuild and ordinary native reopen observe the same state and
  remain writable.

## Risks And Follow-Up

- This closes a live-peer MyLite-owned savepoint rollback boundary, not a
  native InnoDB mid-rollback crash. Native rollback/savepoint rollback internal
  fault coverage remains planned.
- Longer same-table randomized schedules and external MariaDB/RQG-style stress
  remain planned after bounded recovery gates stop exposing new correctness
  issues.

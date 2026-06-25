# Ownerless Savepoint Rollback Before State Crash

## Problem Statement

Ownerless coverage already proves a process killed before `ROLLBACK TO
SAVEPOINT` leaves both pre-savepoint and post-savepoint uncommitted row images
to native InnoDB recovery, and a process killed after a successful savepoint
rollback plus `COMMIT` does not publish a DML file-operation marker for the
rolled-back work.

The remaining MyLite-owned boundary between those cases is after MariaDB has
successfully completed `ROLLBACK TO SAVEPOINT` but before MyLite updates its
process-local savepoint stack and discards pending file-operation redo evidence
for the rolled-back work. If the process dies there, the next ownerless opener
must not turn the rolled-back row image into a durable ownerless page-version
or DML checkpoint marker.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/transaction.cc`
  - `trans_rollback_to_savepoint()` delegates rollback to
    `ha_rollback_to_savepoint()`, then resets the SQL savepoint head.
- `mariadb/sql/handler.cc`
  - `ha_rollback_to_savepoint()` calls each registered engine's
    `savepoint_rollback` callback or rolls back engines registered after the
    savepoint.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  - `innobase_savepoint()` stores the InnoDB undo number in the savepoint
    storage area.
  - `innobase_rollback_to_savepoint()` calls `trx->rollback(savept)` and
    refreshes FTS savepoint state after a successful rollback.
- `packages/libmylite/src/database.cc`
  - MyLite updates process-local savepoint write state only after SQL succeeds
    in `update_ownerless_savepoint_state_after_successful_sql()`.
  - For `ROLLBACK TO SAVEPOINT`, MyLite restores the local-write flag and
    calls `discard_ownerless_native_file_op_redo_after_rolled_back_write()`
    when the rolled-back savepoint contained the only local write.

## Scope And Non-Goals

In scope:

- Add a MyLite unsafe test hook after native `ROLLBACK TO SAVEPOINT` SQL has
  returned successfully but before MyLite mutates ownerless savepoint state.
- Add focused hook-build coverage that kills the writer at that boundary.
- Verify durable dictionary and DML native file-operation markers remain clear.
- Verify ownerless recovery, forced `.shm` rebuild, and ordinary native reopen
  all preserve the pre-transaction row state and allow a follow-up native
  write.

Out of scope:

- Crash injection inside MariaDB/InnoDB's `trx->rollback(savept)` routine.
- Concurrent-writer savepoint schedules.
- DDL/file-lifecycle recovery or SQL-level table-lock fault injection.
- New production behavior, file formats, public API, or native storage changes.

## Design

Add `pause_for_ownerless_test_fault("savepoint-rollback-before-state")` inside
the successful `ROLLBACK TO SAVEPOINT` branch of
`update_ownerless_savepoint_state_after_successful_sql()`, after MyLite has
parsed the savepoint name but before it searches and mutates the process-local
savepoint stack. This keeps the hook at a stable MyLite-owned boundary:
MariaDB has already completed the native rollback, and MyLite has not yet
discarded process-local redo evidence.

The hook test creates an InnoDB table, checkpoints the setup, starts an
explicit ownerless transaction, creates a savepoint, updates a file-per-table
row after the savepoint, confirms local file-op redo evidence exists, and then
executes `ROLLBACK TO SAVEPOINT` with the hook armed. The parent kills the
writer when the hook signals readiness.

Recovery expectations are deliberately conservative: because the transaction
never commits and the process dies before close, both durable native file-op
markers must remain clear, and all reopen paths must observe the original row.

## Compatibility Impact

Successful SQL behavior is unchanged. The hook is active only in unsafe hook
builds when the named test fault is requested. The slice expands crash evidence
for MyLite's ownerless transaction cleanup around a supported MariaDB
savepoint rollback.

## Directory And Lifecycle Impact

No directory layout changes. The killed child exits without `mylite_close()`;
the next ownerless opener must clean stale ownerless process state using the
database directory's durable coordination files and MariaDB native recovery.

## Native Storage Impact

No storage-format changes. InnoDB undo remains authoritative for both the
rolled-back savepoint update and the uncommitted transaction outcome after the
writer is killed.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive build-profile
changes. The implementation adds one unsafe test hook and focused hook SQL
coverage only.

## Test And Verification Plan

- Build hook target `mylite_ownerless_cross_process_sql_test`.
- Run focused hook selector
  `savepoint-rollback-before-state-crash`.
- Run adjacent transaction marker selectors:
  `native-killed-before-savepoint-rollback-dml-file-op-marker-recovery`,
  `native-killed-savepoint-dml-file-op-marker-recovery`,
  `native-explicit-dml-savepoint-file-op-marker-discard`, and
  `native-explicit-dml-savepoint-file-op-marker-retain`.
- Run the registered hook CTest entry.
- Build production ownerless SQL target and run the adjacent production
  selectors to prove the hook does not affect non-hook builds.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- The writer reaches the hook after native `ROLLBACK TO SAVEPOINT` succeeds and
  before MyLite updates ownerless savepoint state.
- Killing the writer there leaves both durable native file-operation markers
  clear.
- The next ownerless opener observes the original row value and payload,
  checkpoints recovery state, and leaves both markers clear.
- Forced `.shm` rebuild and ordinary native reopen observe the same original
  row and allow a follow-up write.
- Adjacent killed-before-rollback, killed-after-savepoint-commit, and live
  savepoint marker selectors continue to pass.

## Verification Results

- Hook build: `savepoint-rollback-before-state-crash`.
- Hook build adjacent selectors:
  `native-killed-before-savepoint-rollback-dml-file-op-marker-recovery`,
  `native-killed-savepoint-dml-file-op-marker-recovery`,
  `native-explicit-dml-savepoint-file-op-marker-discard`, and
  `native-explicit-dml-savepoint-file-op-marker-retain`.
- Hook CTest:
  `libmylite.ownerless-savepoint-rollback-before-state-crash`.
- Production embedded build adjacent selectors:
  `native-killed-before-savepoint-rollback-dml-file-op-marker-recovery`,
  `native-killed-savepoint-dml-file-op-marker-recovery`,
  `native-explicit-dml-savepoint-file-op-marker-discard`, and
  `native-explicit-dml-savepoint-file-op-marker-retain`.
- Guards: `format-check`, `tools/check-ci-production-builds`,
  `tools.ci-production-builds`, and `git diff --check`.

## Risks And Follow-Up

- This is a MyLite-owned post-native rollback boundary, not a crash inside
  InnoDB's savepoint rollback routine.
- Concurrent-writer savepoint crash schedules remain open.
- Broader DDL/file-lifecycle recovery, active-reader pressure crash/oracle
  breadth, and randomized external MariaDB/RQG stress remain open
  ownerless-concurrency completion work.

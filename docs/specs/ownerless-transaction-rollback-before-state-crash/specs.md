# Ownerless Transaction Rollback Before State Crash

## Problem Statement

Ownerless coverage proves successful explicit-transaction rollback discards
process-local DML file-operation evidence, and killed explicit transactions
roll back through native InnoDB recovery without publishing uncommitted rows.
The remaining MyLite-owned boundary between those cases is after MariaDB has
successfully completed a transaction-ending `ROLLBACK` but before MyLite resets
its process-local ownerless transaction state, releases ownerless transaction
pins/locks, and discards rollback-only native file-operation evidence.

If a process dies at that boundary, the next ownerless opener must clean stale
ownerless state and must not turn the rolled-back row image into durable
page-version WAL or a DML checkpoint marker.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/transaction.cc:371-403` implements `trans_rollback()`: it
  calls `ha_rollback_trans(thd, TRUE)`, clears transaction server status and
  option bits, resets THD transaction state, and tracks transaction end.
- `mariadb/sql/handler.cc:2614-2760` implements `ha_rollback_trans()`: it
  calls each registered storage engine rollback participant and clears the
  transaction participant list.
- `mariadb/storage/innobase/handler/ha_innodb.cc:4803-4889` implements
  `innobase_rollback()`: a full transaction rollback delegates to
  `trx_rollback_for_mysql(trx)` and deregisters the transaction from 2PC.
- `mariadb/storage/innobase/trx/trx0roll.cc:224-252` implements
  `trx_rollback_for_mysql()`: active transactions roll back through
  `trx->rollback_low()`.
- `packages/libmylite/src/database.cc` updates ownerless transaction state only
  after SQL succeeds in `update_ownerless_transaction_state_after_successful_sql()`;
  rollback-only DML evidence is then refreshed/discarded by the ownerless SQL
  success path.

## Scope And Non-Goals

In scope:

- Add an unsafe MyLite hook after native transaction-ending `ROLLBACK` SQL has
  returned successfully but before MyLite mutates ownerless transaction state.
- Add focused hook-build coverage that kills the writer at that boundary.
- Verify durable dictionary and DML native file-operation markers remain clear.
- Verify ownerless recovery, forced `.shm` rebuild, and ordinary native reopen
  preserve the pre-transaction row state and allow a follow-up native write.

Out of scope:

- Crash injection inside MariaDB/InnoDB rollback internals.
- `ROLLBACK AND CHAIN` state carry-over semantics.
- Concurrent-writer rollback schedules or same-page savepoint contention.
- DDL/file-lifecycle recovery, SQL-level table-lock fault injection, or
  external MariaDB/RQG stress expansion.

## Design

Add `pause_for_ownerless_test_fault("transaction-rollback-before-state")` in
the transaction-ending rollback branch of
`update_ownerless_transaction_state_after_successful_sql()`, after MariaDB has
returned success and before MyLite releases the transaction page-version pin,
clears ownerless transaction state, resets savepoints, or refreshes rollback
visibility. The hook is compiled only for unsafe hook builds and only pauses
when the named fault is configured.

The hook test creates a file-per-table InnoDB table, checkpoints setup, starts
an ownerless explicit transaction, updates a row after the checkpoint, confirms
local native file-operation redo evidence exists, and executes `ROLLBACK` with
the hook armed. The parent kills the writer when the hook signals readiness.

Recovery expectations are conservative: the transaction never commits, both
durable native file-operation markers remain clear, ownerless recovery sees the
original row, and forced `.shm` rebuild plus ordinary native reopen observe the
same original row before accepting a follow-up write.

## Compatibility Impact

Successful SQL behavior is unchanged. The hook is inactive in production
builds and inactive in hook builds unless the named fault is requested. The
slice expands crash evidence for MyLite's ownerless cleanup around a supported
MariaDB explicit transaction rollback.

## Directory And Lifecycle Impact

No directory layout changes. The killed child exits without `mylite_close()`;
the next ownerless opener must clean stale ownerless process state using the
database directory's durable coordination files and MariaDB native recovery.

## Native Storage Impact

No native storage format change. InnoDB undo remains authoritative for the
rolled-back transaction row image; MyLite only proves that stale process-local
ownerless state does not publish that image after process death.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive build-profile
change. The implementation adds one unsafe test hook and focused hook SQL
coverage only.

## Test And Verification Plan

- Build hook target `mylite_ownerless_cross_process_sql_test`.
- Run focused hook selector `transaction-rollback-before-state-crash`.
- Run adjacent transaction marker selectors:
  `native-killed-dml-file-op-marker-recovery`,
  `native-killed-uncommitted-dml-file-op-marker-recovery`,
  `native-explicit-dml-rollback-file-op-marker-discard`,
  `native-killed-before-savepoint-rollback-dml-file-op-marker-recovery`, and
  `savepoint-rollback-before-state-crash`.
- Run the registered hook CTest entry.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- The writer reaches the hook after native transaction-ending `ROLLBACK`
  succeeds and before MyLite resets ownerless transaction state.
- Killing the writer there leaves both durable native file-operation markers
  clear.
- The next ownerless opener observes the original row value and payload,
  checkpoints recovery state, and leaves both markers clear.
- Forced `.shm` rebuild and ordinary native reopen observe the same original
  row and allow a follow-up write.
- Adjacent killed-transaction, successful rollback, and savepoint rollback
  selectors continue to pass.

## Risks And Follow-Up

- This is a MyLite-owned post-native rollback boundary, not a crash inside
  InnoDB's full rollback routine.
- `ROLLBACK AND CHAIN`, arbitrary concurrent-writer rollback schedules,
  same-page savepoint contention, broader native redo/checkpoint
  reconciliation, DDL/file-lifecycle recovery, active-reader pressure breadth,
  and randomized external MariaDB/RQG stress remain separate ownerless
  completion work.

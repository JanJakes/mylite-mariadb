# Ownerless Native Row Undo Rollback Crash

## Problem Statement

Ownerless rollback crash coverage currently kills writers either before a
savepoint rollback begins or after native rollback has already returned to the
MyLite SQL wrapper. The remaining higher-risk transaction boundary is a process
death while InnoDB native row undo is actively applying rollback records. MyLite
must let native recovery finish the uncommitted transaction, must not publish
partially rolled-back page images as committed ownerless WAL, and must clear
rollback-only DML file-operation evidence after recovery.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0roll.cc` implements
  `trx_t::rollback_low()`. It builds the rollback graph, runs
  `que_run_threads(roll_node->undo_thr)`, then performs full-rollback or
  savepoint cleanup.
- `mariadb/storage/innobase/row/row0undo.cc` implements the shared
  `row_undo()` path. It fetches one undo record through `row_undo_rec_get()`,
  applies either `row_undo_ins()` or `row_undo_mod()`, releases the undo page,
  and leaves the undo node ready for the next record.
- `packages/libmylite/src/database.cc` repairs ownerless transaction/savepoint
  state after successful SQL returns. Existing `transaction-rollback-before-state`
  and `savepoint-rollback-before-state` hooks cover that post-native boundary
  but intentionally do not fault inside InnoDB row undo.
- Existing ownerless explicit-transaction code clears transaction-deferred page
  images for full rollback and marks savepoint rollback as page-publication
  failed, so committed page-version publication remains a COMMIT-only proof.

## Design

Add an unsafe test-only fault in `row_undo()` after a successful native row undo
operation and after the undo page/persistent cursor are released. The hook name
is `rollback-after-native-row-undo`. It is gated by the existing
`mylite_ownerless_innodb_test_faults_enabled_fast()` flag and is dormant in
production builds unless the unsafe hook build enables test faults.

Add two focused hook-build crash selectors:

1. `savepoint-rollback-native-row-undo-crash`
   - Start an ownerless explicit transaction.
   - Update one row before a savepoint.
   - Update two rows after the savepoint so native rollback has multiple row
     undo records available.
   - Arm `rollback-after-native-row-undo`, execute
     `ROLLBACK TO SAVEPOINT`, kill the writer after the first successful native
     row undo, and verify no-live recovery rolls the whole uncommitted
     transaction back to the original rows.
2. `transaction-rollback-native-row-undo-crash`
   - Start an ownerless explicit transaction.
   - Update two rows.
   - Arm `rollback-after-native-row-undo`, execute `ROLLBACK`, kill after the
     first successful native row undo, and verify recovery preserves the
     original rows.

Both selectors verify ownerless recovery, forced `.shm` rebuild, ordinary
native reopen, writable follow-up DML, clear native DML/file-operation markers,
and eventual ownerless WAL checkpointing.

## Scope And Non-Goals

In scope:

- One row-undo crash boundary shared by full rollback and savepoint rollback.
- File-per-table InnoDB tables with ordinary row updates.
- No-live ownerless recovery, forced shared-memory rebuild, and ordinary native
  reopen.

Out of scope:

- Exhaustive arbitrary crash fuzzing at every row-undo substep.
- Foreign-key action rollback, trigger side effects, generated-column side
  effects, DDL rollback, XA rollback, or prepared transactions.
- Concurrent live-peer recovery while native row undo is paused.
- Performance changes to native undo or ownerless page publication.

## Compatibility Impact

No SQL syntax, public C API, or production behavior changes. The slice
preserves MariaDB/InnoDB transaction semantics: a process killed inside
rollback still leaves an uncommitted transaction, and native recovery rolls it
back completely.

## Directory, Lifecycle, And Native Storage Impact

No durable directory-layout change and no native storage-format change. The
test proves the next ownerless opener can recover a killed writer whose native
undo may have partially modified table pages, without retaining stale DML
file-operation markers or treating partial rollback images as committed
ownerless page-version payload.

## Public API, Build, Size, And License

No public API, dependency, license, or binary-size-sensitive profile change.
The only MariaDB-derived source change is a narrow unsafe test-fault hook in
`row0undo.cc`; the hook is inert without the hook-build test-fault flag.

## Test And Verification Plan

- Configure and build `ownerless-test-hooks`.
- Run direct selectors:
  `savepoint-rollback-native-row-undo-crash` and
  `transaction-rollback-native-row-undo-crash`.
- Run adjacent rollback hook CTests covering pre-native, native-row-undo, and
  post-native/pre-MyLite-state boundaries.
- Run the relevant production ownerless transaction/savepoint selectors.
- Run `format-check-prod`, `git diff --check`, and CI-production-build guards.

## Acceptance Criteria

- The hook fires only after at least one native row undo succeeds.
- Killing the writer at that point leaves original durable rows after no-live
  ownerless recovery.
- Native DML/file-operation markers remain clear after recovery.
- Ownerless WAL checkpoints after recovery and stays correct after forced
  `.shm` rebuild.
- Ordinary native reopen observes the same recovered state and accepts a
  follow-up write.
- Existing rollback/savepoint hook selectors still pass.

## Risks And Follow-Up

- This covers one deterministic row-undo boundary, not every sub-operation
  inside `row_undo_ins()` or `row_undo_mod()`.
- Live-peer recovery while a writer dies inside native rollback internals,
  longer randomized savepoint schedules, foreign-key/trigger rollback side
  effects, broader redo/checkpoint reconciliation, and external MariaDB/RQG
  stress remain completion work.

# Ownerless Savepoint Prewrite Rollback Crash

## Problem Statement

Existing ownerless rollback crash coverage kills `ROLLBACK TO SAVEPOINT` after
native rollback succeeds but before MyLite refreshes ownerless transaction state
for a savepoint that was taken before any local write. A remaining transaction
window is a savepoint rollback where the transaction already had an earlier
same-table write before the savepoint. If that process dies at the same
post-native/pre-MyLite-state boundary, native recovery must roll back the whole
uncommitted transaction and MyLite must not retain stale DML file-operation
markers or page-version state.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- InnoDB owns the transactional rollback semantics. A process death before
  commit leaves the explicit transaction uncommitted, even if
  `ROLLBACK TO SAVEPOINT` already removed later changes inside that
  transaction.
- `packages/libmylite/src/database.cc`
  `update_ownerless_savepoint_state_after_successful_sql()` updates MyLite's
  savepoint write-state stack only after MariaDB reports successful SQL
  execution. The existing unsafe hook `savepoint-rollback-before-state` pauses
  after native rollback and before MyLite restores/discards the tracked local
  write state.

## Design

Reuse the existing `savepoint-rollback-before-state` hook. Add a focused
hook-build selector that:

1. Creates a two-row InnoDB table.
2. Starts an ownerless explicit transaction.
3. Updates row 1 before a savepoint.
4. Creates the savepoint.
5. Updates row 2 after the savepoint.
6. Executes `ROLLBACK TO SAVEPOINT`, then pauses at the existing hook before
   MyLite updates ownerless savepoint state.
7. Kills the writer process.

Recovery should observe the original two rows, no native file-operation marker,
checkpointed ownerless WAL, correct forced-`.shm` rebuild behavior, and ordinary
native writeability after recovery.

## Scope And Non-Goals

In scope:

- Same-table two-row explicit InnoDB transaction.
- One pre-savepoint write and one post-savepoint write.
- Crash after native savepoint rollback succeeds and before MyLite ownerless
  savepoint state is updated.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Committed savepoint transactions.
- Cross-process interleavings while the killed transaction is paused.
- Foreign-key, trigger, generated-column, and DDL side effects.
- Native InnoDB internal row-undo fault injection.

## Compatibility Impact

No SQL syntax or public API behavior changes. This preserves MariaDB/InnoDB
transaction semantics: a killed uncommitted transaction rolls back completely,
including writes that occurred before the savepoint.

## Directory, Lifecycle, And Native Storage Impact

No durable layout change. The test proves ownerless recovery does not leave
stale DML file-operation markers or uncheckpointed ownerless WAL after native
rollback removes all uncommitted row changes.

## Public API, Build, Size, License

No public API, dependency, binary-size, or license changes. The slice adds only
hook-build test coverage and documentation.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run direct selector `savepoint-rollback-prewrite-before-state-crash`.
- Run adjacent rollback/savepoint hook CTests.
- Run production formatting and CI-production-build guards.
- Run `git diff --check`.

## Acceptance Criteria

- The writer pauses after native `ROLLBACK TO SAVEPOINT` and before MyLite
  updates ownerless savepoint state.
- Killing the writer leaves the original row values and payloads after ownerless
  recovery.
- Native DML and DDL file-operation markers stay clear.
- Ownerless WAL checkpoints after recovery.
- Forced `.shm` rebuild and ordinary native reopen observe the same original
  state and remain writable.

## Risks And Follow-Up

- This is still a SQL-layer post-native hook, not an InnoDB row-undo internal
  fault. Broader native rollback internals remain planned.
- Cross-process savepoint schedules that keep another writer active while the
  rollback owner dies remain planned.

# Ownerless Savepoint DML Marker Retain

## Problem

Ownerless savepoint DML marker coverage proved the branch where
`ROLLBACK TO SAVEPOINT` removes all handle-local writes from an explicit
transaction and therefore discards process-local native file-operation redo
before a later `COMMIT`.

The opposite branch also needs evidence: if a checkpointed ownerless DML write
already existed before the savepoint, rolling back a later write to that
savepoint must not discard the file-operation evidence that still belongs to
the surviving transaction outcome. The later `COMMIT` must publish the
DML-specific native file-operation checkpoint marker for that surviving write.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `SAVEPOINT ident`,
  `ROLLBACK [WORK] TO [SAVEPOINT] ident`, and
  `RELEASE SAVEPOINT ident` into dedicated SQL commands.
- `mariadb/sql/transaction.cc:trans_savepoint()` registers a savepoint with
  participating storage engines. `trans_rollback_to_savepoint()` delegates row
  undo to `ha_rollback_to_savepoint()`, makes the target savepoint the
  transaction savepoint head, and deletes later savepoints.
- `mariadb/sql/handler.cc:ha_rollback_to_savepoint()` rolls participating
  engines back to the savepoint and fully rolls back engines that joined after
  the savepoint.
- `mariadb/storage/innobase/handler/ha_innodb.cc:innobase_savepoint()` stores
  the InnoDB undo number in the savepoint area, and
  `innobase_rollback_to_savepoint()` rolls the transaction back to that undo
  number.
- `packages/libmylite/src/database.cc` snapshots MyLite's ownerless
  `ownerless_transaction_has_local_write` flag at `SAVEPOINT`, restores it at
  `ROLLBACK TO`, and discards process-local native file-operation redo only
  when no earlier local write survives the rollback.

## Design

Add a focused production SQL selector for the retained-earlier-write branch:

- create a file-per-table InnoDB table and checkpoint the database,
- start an ownerless explicit transaction,
- update row 1 after the checkpoint and prove the process-local native
  file-operation redo flag is set,
- create a savepoint,
- update row 2 after the savepoint and prove the flag is set again,
- `ROLLBACK TO SAVEPOINT`,
- require the process-local file-operation redo flag to remain set because row
  1 still belongs to the transaction outcome,
- commit and require the DML-specific native file-operation checkpoint marker
  to be durable,
- close with no live peer and require native checkpoint proof to drain the
  marker and WAL,
- reopen ownerless after forced `.shm` rebuild and reopen ordinary native to
  prove row 1 survived, row 2 rolled back, and the table remains writable.

The slice adds no product code unless the selector exposes a regression.
MariaDB remains authoritative for savepoint row undo; MyLite only proves its
ownerless checkpoint evidence classification matches the native outcome.

## Compatibility Impact

SQL behavior remains MariaDB-owned. The new evidence covers MyLite's internal
checkpoint-marker behavior for supported explicit transactions with savepoints.
The externally visible result is that a committed pre-savepoint write remains
durable and recoverable after a later post-savepoint write is rolled back.

## Directory And Lifecycle Impact

No file format or directory layout changes are introduced. The test asserts the
DML marker exists only between the successful `COMMIT` and the final no-live
checkpoint drain, and that ownerless plus ordinary native reopen paths need no
manual cleanup.

## Native Storage Impact

InnoDB undo remains the source of truth for row contents. MyLite retains the
process-local file-operation redo latch only as durable checkpoint evidence for
the surviving earlier write.

## Binary Size And Dependencies

The slice adds one SQL test selector, one focused CTest entry, and docs. It
does not add dependencies or change the embedded MariaDB profile.

## Test Plan

- Add `native-explicit-dml-savepoint-file-op-marker-retain`.
- Register `libmylite.ownerless-explicit-dml-savepoint-marker-retain`.
- Run the new selector directly.
- Run adjacent DML marker selectors for savepoint discard, rollback discard,
  deadlock discard, single-owner explicit DML marker drain, multi-peer explicit
  DML marker retention/drain, and killed savepoint recovery.
- Run the focused production CTest filter, `format-check-prod`, and
  `git diff --check`.

## Acceptance Criteria

- `ROLLBACK TO SAVEPOINT` after a later checkpointed ownerless DML write leaves
  process-local native file-operation redo set when an earlier local write
  survives the rollback.
- The later `COMMIT` publishes the DML-specific native file-operation
  checkpoint marker.
- No generic dictionary/file-lifecycle marker is written for the DML-only
  transaction.
- No-live close drains the DML marker and WAL after native checkpoint proof.
- Ownerless reopen after forced `.shm` rebuild and ordinary native reopen see
  the surviving pre-savepoint row image and not the rolled-back row image.

## Risks

- This is a successful live-transaction branch proof. Killed processes or
  injected crashes during savepoint rollback, before rollback, or amid
  concurrent writers remain separate matrices.
- A single file-per-table `UPDATE` shape represents checkpointed DML
  `FILE_MODIFY` evidence; broader DML-origin file-operation classes remain
  planned.

# Ownerless Savepoint DML Marker Discard

## Problem

Ownerless DML file-operation markers represent committed DML evidence that must
be drained through native checkpoint proof. Successful rollback and deadlock
victim paths already discard process-local file-operation redo, but
`ROLLBACK TO SAVEPOINT` kept MyLite's handle-local
`ownerless_transaction_has_local_write` flag set even when all writes since the
savepoint were undone.

That was conservative for recovery because the DML-specific marker does not
relax user-page proof, but it made a later `COMMIT` able to publish a committed
DML marker for work that no longer belonged to the transaction outcome. This
slice tightens the explicit-transaction savepoint classification.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `SAVEPOINT ident`,
  `ROLLBACK [WORK] TO [SAVEPOINT] ident`, and
  `RELEASE SAVEPOINT ident` into `SQLCOM_SAVEPOINT`,
  `SQLCOM_ROLLBACK_TO_SAVEPOINT`, and `SQLCOM_RELEASE_SAVEPOINT`.
- `mariadb/sql/sql_parse.cc` dispatches those commands to
  `trans_savepoint()`, `trans_rollback_to_savepoint()`, and
  `trans_release_savepoint()`.
- `mariadb/sql/transaction.cc:trans_savepoint()` adds a named savepoint and
  records the MDL savepoint. `trans_rollback_to_savepoint()` rolls storage
  engines back to the named savepoint, makes that savepoint the head of the
  transaction savepoint list, and releases MDL acquired after it when safe.
  `trans_release_savepoint()` removes the named savepoint.
- `mariadb/sql/handler.cc:ha_savepoint()` and
  `ha_rollback_to_savepoint()` call storage-engine savepoint hooks.
- `mariadb/storage/innobase/handler/ha_innodb.cc:innobase_savepoint()` stores
  the transaction undo number, and `innobase_rollback_to_savepoint()` rolls
  InnoDB state back to that undo number.
- `packages/libmylite/src/database.cc` snapshots
  `ownerless_transaction_has_local_write` before `COMMIT` and uses that to
  publish DML-specific file-operation checkpoint-needed evidence. Full
  `ROLLBACK` and deadlock rollback already consume process-local file-op redo
  for rolled-back local writes.

## Design

Track a small handle-local savepoint write-state stack in `mylite_db`:

- `SAVEPOINT name` records whether the ownerless explicit transaction already
  had local writes when the savepoint was created. Reusing a savepoint name
  replaces the earlier tracked entry.
- `ROLLBACK TO [SAVEPOINT] name` restores
  `ownerless_transaction_has_local_write` from the tracked savepoint entry and
  discards process-local ownerless InnoDB file-op redo when no local write
  survives the rollback.
- `ROLLBACK TO` drops tracked savepoints created after the target, matching
  MariaDB's transaction savepoint list behavior.
- `RELEASE SAVEPOINT name` removes the tracked entry and newer tracked
  savepoints without changing the transaction write flag, matching MariaDB's
  transaction savepoint head update.
- Full transaction start/end clears the stack.

The implementation stays in MyLite handle state. It does not alter MariaDB
savepoint execution, native InnoDB redo, MyLite WAL formats, checkpoint record
formats, or public APIs.

## Compatibility Impact

SQL behavior and diagnostics remain MariaDB-owned. The only behavior change is
internal ownerless checkpoint evidence classification: a later `COMMIT` no
longer publishes DML-specific checkpoint-needed evidence solely because a
post-savepoint write was rolled back.

## Directory And Lifecycle Impact

No files or directory layout are added. The change reduces unnecessary
`concurrency/mylite-concurrency.ckpt` DML marker publication for rolled-back
savepoint work and keeps existing no-live checkpoint drain semantics for
committed writes.

## Native Storage Impact

Native InnoDB savepoint rollback remains authoritative for row contents and
undo. MyLite only aligns its file-operation marker classification with the
native savepoint outcome.

## Binary Size And Dependencies

The slice adds a small `std::vector` of per-handle savepoint write states and a
focused SQL test. No dependency or embedded MariaDB profile change is needed.

## Test Plan

- Add a focused `native-explicit-dml-savepoint-file-op-marker-discard`
  selector.
- The selector forces a native checkpoint, clears the process-local file-op
  latch, performs checkpointed DML after a savepoint, proves the process-local
  file-op latch was set, rolls back to the savepoint, commits, and asserts no
  DML marker was persisted.
- The selector then verifies the pre-savepoint row state through ownerless
  reopen, forced `.shm` rebuild, ordinary native reopen, and a follow-up native
  write.
- Run the focused selector with adjacent rollback, deadlock, single-owner, and
  multi-peer DML marker selectors.
- Run relevant production CTest filters, ownerless hook smoke where needed,
  `format-check-prod`, and `git diff --check`.

## Acceptance Criteria

- `ROLLBACK TO SAVEPOINT` after ownerless DML restores the handle-local local
  write flag when no earlier write survives the savepoint.
- Process-local ownerless file-op redo from the rolled-back savepoint work is
  discarded before the later `COMMIT`.
- The later `COMMIT` leaves both dictionary and DML file-op checkpoint markers
  clear.
- The rolled-back row image is not visible through ownerless/native reopen or
  forced `.shm` rebuild.
- Existing full rollback, deadlock, single-owner commit, and multi-peer commit
  DML marker coverage continues to pass.
- A follow-up killed-session selector proves the same marker discard after a
  successful later `COMMIT` followed by `_exit(0)` before `mylite_close()`.

## Risks

- This slice tracks user-visible savepoint statements in the MyLite SQL layer.
  It does not track internal MariaDB statement savepoints, which remain native
  implementation details and do not become user transaction outcomes.
- Savepoint marker classification is still bounded evidence, not exhaustive
  mid-rollback crash or concurrent-writer savepoint matrix coverage.

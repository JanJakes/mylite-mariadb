# Transaction Context Active Statement Hint

## Roadmap Slice

- Row and index storage
- Transactions and recovery
- Spec slug: `transaction-context-active-statement-hint`

## Source Authority

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Relevant MyLite handler/storage paths:
  - `mariadb/storage/mylite/ha_mylite.cc`
  - `packages/mylite-storage/src/storage.c`

## Problem

`ha_mylite::external_lock()` runs on every row-DML execution path. Inside an
explicit transaction it asks the storage layer whether a statement/checkpoint is
already active before deciding whether to start the durable transaction
checkpoint. Once the handler transaction context already owns a durable
transaction checkpoint, that storage active-statement lookup is redundant.

The redundant lookup walks storage active-statement state by filename and is
visible in hot prepared update samples.

## Design

Use the handler transaction context as the first active-statement proof:

- when the current THD already has `ctx->transaction`, treat the storage
  statement as known active;
- otherwise preserve the existing `mylite_storage_statement_active()` check and
  transaction-checkpoint startup path.

This keeps the storage-layer lookup for the first statement in a transaction,
for any context without an established durable checkpoint, and for defensive
fallbacks where handler state does not prove storage ownership.

## Compatibility Impact

No SQL-visible behavior changes. The change only reuses handler-owned
transaction state to skip a redundant storage-chain lookup after the durable
transaction checkpoint exists.

## File Lifecycle Impact

None. The change does not alter journal creation, checkpoint ownership, or file
locking behavior.

## Test Plan

- Rebuild the MyLite storage-smoke MariaDB archive with
  `-DPLUGIN_MYLITE_SE=STATIC`.
- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline`.
- Run `mylite_storage_test`.
- Run the storage and embedded storage-engine CTest subset.
- Run `git diff --check`.
- Run `git clang-format --diff HEAD -- mariadb/storage/mylite/ha_mylite.cc`.
- Run a local prepared-update performance baseline as a sanity check.

## Acceptance Criteria

- Explicit transaction row-DML still opens, commits, and rolls back through the
  existing transaction coverage.
- Autocommit statement checkpoints still start through the existing path.
- Prepared update samples no longer need the storage active-statement filename
  lookup after the transaction checkpoint is established.

## Risks

- The hint must be limited to an existing handler-owned durable transaction
  checkpoint. It must not skip the storage lookup merely because a volatile
  snapshot or savepoint frame exists.

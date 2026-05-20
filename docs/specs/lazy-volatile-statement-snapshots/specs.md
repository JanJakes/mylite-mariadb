# Lazy Volatile Statement Snapshots

## Problem

Every MyLite write lock currently creates a volatile-table statement snapshot,
even when the handler instance is writing a durable MyLite table. The prepared
primary-key update benchmark writes only durable rows, so `external_lock()`
spends time copying MEMORY/HEAP snapshot state that cannot be used by that
statement.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), with this slice limited to the
  MyLite handler and volatile-row runtime.
- `ha_mylite::external_lock()` knows whether the current handler uses volatile
  rows through `volatile_rows`.
- `mylite_begin_statement_checkpoint()` always calls
  `mylite_volatile_begin_snapshot()`, even for durable-only handlers.
- `mylite_begin_transaction_checkpoint()` creates the transaction-level
  volatile snapshot eagerly. This is retained because explicit transaction
  boundaries can span later volatile writes.
- Savepoint creation must stay eager because a later volatile write needs the
  savepoint snapshot from the exact `SAVEPOINT` boundary.

## Design

- Pass a volatile-snapshot requirement flag from `external_lock()` into
  statement checkpoint startup.
- Durable handlers still create durable statement checkpoints as before, but
  skip volatile statement snapshot creation.
- Volatile handlers create the missing statement volatile snapshot on first
  volatile write lock.
- Statement checkpoint startup checks the existing handler transaction context
  before asking storage whether a statement is already active.
- Leave transaction and savepoint snapshots unchanged and eager.

## Compatibility Impact

No SQL, C API, storage-engine routing, file-format, or durability behavior
changes. Durable row rollback remains handled by MyLite storage checkpoints.
MEMORY/HEAP statement, transaction, and savepoint rollback semantics remain
covered.

## Single-File And Lifecycle Impact

No durable file lifecycle changes. Volatile snapshots are process-owned runtime
state and still finish through the existing commit/rollback paths.

## Tests And Verification

- Keep mixed durable-then-volatile transaction rollback coverage to prove the
  narrowed statement-snapshot optimization does not disturb eager transaction
  snapshots.
- Run:
  - `git diff --check`
  - `git clang-format --diff`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --preset storage-smoke-dev`
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_statement_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `build/storage-smoke-dev/packages/libmylite/mylite_embedded_statement_test`
  - `build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`
  - `ctest --preset storage-smoke-dev --output-on-failure`
  - `tools/mylite-perf-baseline --phase=prepared-updates 1000 1000000`

## Acceptance Criteria

- Durable-only point updates do not call statement-level
  `mylite_volatile_begin_snapshot()` from `external_lock()`.
- Volatile writes inside an explicit transaction still roll back.
- Existing MEMORY/HEAP savepoint rollback remains unchanged.
- Storage-smoke tests pass.

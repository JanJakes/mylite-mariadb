# Nested Statement Checkpoint Begin

## Problem

Prepared row-DML inside an explicit transaction begins a nested storage
statement checkpoint for every execution so failed statements can roll back to
the transaction-local pre-statement state. The storage begin path currently
receives only a filename, then rediscovers the active parent statement by
walking the active statement chain and comparing filenames.

`libmylite` already stores the active transaction statement pointer on the
database handle. For nested row-DML checkpoints in that transaction, the
filename lookup is redundant and appears in prepared-update samples as
`active_statement_for()` / `strcmp()` under checkpoint begin.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This slice is first-party MyLite work in
  `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/include/mylite/storage.h`, and
  `packages/libmylite/src/database.cc`; no upstream MariaDB file changes.
- `begin_checkpoint()` already has a parent fast path after it finds the active
  statement: nested checkpoints reuse the parent's file and filename, clone the
  parent checkpoint snapshot, and skip durable snapshot reads.
- `StorageStatementCheckpoint::begin()` is called for prepared and direct
  statement checkpoints; during active transactions `mylite_db::transaction_statement`
  names the outer storage checkpoint, and the savepoint stack names any more
  recent active parent.
- The current prepared-update benchmark samples nested begin work under
  `active_statement_for()` filename comparisons and `clone_parent_checkpoint_snapshot()`.

## Design

- Add an internal storage API that begins a nested statement checkpoint from an
  already-known parent `mylite_storage_statement *`.
- Require the supplied parent to be the current active statement and to belong
  to the current storage context owner. This preserves the existing single
  active-stack discipline and rejects stale or cross-handle parent pointers.
- Reuse the same nested allocation, parent file, parent filename, snapshot
  clone, and active-statement push behavior as `begin_checkpoint()`.
- Update `StorageStatementCheckpoint::begin()` and direct savepoint creation to
  use the direct nested API when an active transaction or savepoint parent is
  available; retain the filename API for top-level statement checkpoints.
- Keep volatile snapshot handling unchanged.

## Compatibility Impact

No SQL, public `libmylite` API, file-format, metadata, or storage-engine
routing changes. This narrows an internal storage checkpoint entry point.

## Single-File And Lifecycle Impact

No durable lifecycle changes. Nested statement checkpoints still share the
transaction file handle and rollback state, and still commit or roll back
through the existing statement close path.

## Binary-Size Impact

Negligible. The change adds one small storage entry point and no dependencies.

## Tests And Verification

- Run:
  - `git diff --check`
  - `git clang-format --diff`
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_statement_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `build/storage-smoke-dev/packages/libmylite/mylite_embedded_statement_test`
  - `build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`
  - `ctest --preset storage-smoke-dev --output-on-failure`
  - `tools/mylite-perf-baseline --phase=prepared-updates 1000 1000000`

## Acceptance Criteria

- Transaction-local statement checkpoints can begin from the known transaction
  or savepoint parent without filename lookup.
- Stale, null, or non-active parent pointers reject with storage misuse.
- Existing transaction, savepoint, rollback, and prepared row-DML tests pass.
- The prepared-update benchmark completes with the expected checksum, and local
  sampling no longer shows nested checkpoint begin dominated by filename
  comparison.

## Risks And Unresolved Questions

- The new entry point must remain internal to storage/libmylite usage; callers
  must not retain parent pointers beyond their active checkpoint lifetime.
- The largest remaining benchmark cost is still MariaDB per-execute planning.

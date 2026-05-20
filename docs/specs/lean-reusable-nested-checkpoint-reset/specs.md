# Lean Reusable Nested Checkpoint Reset

## Problem Statement

Prepared row-DML samples still show `initialize_nested_checkpoint_storage()`
while committing each nested storage checkpoint. The reusable nested checkpoint
path needs to clear lifecycle flags before caching the object, but it does not
need the full fresh-allocation initializer: `free_statement()` has already
released and zeroed owned cache fields, and the next nested begin clones the
current parent header/catalog snapshot.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite first-party storage owns nested statement checkpoint reuse in
  `packages/mylite-storage/src/storage.c`.
- `free_statement()` clears exact-index, live-row, live-row-id, row-payload,
  table-entry, append-page, buffered-undo, and buffered-rewrite owned state
  before deciding whether a nested checkpoint can be cached.
- `initialize_checkpoint_statement()` then overwrites parent-owned nested
  snapshot fields by calling `clone_parent_checkpoint_snapshot()`.
- The reusable cached object therefore only needs a small lifecycle reset for
  ownership, journal, deferred-cache-retarget, and identity flags.

## Proposed Design

- Add a reset helper for reusable nested checkpoints.
- Keep `initialize_nested_checkpoint_storage()` for freshly allocated nested
  checkpoint objects.
- When caching a reusable nested checkpoint, call the narrower reset helper
  instead of the fresh-allocation initializer.
- Leave owned cache cleanup in `free_statement()` as the single ownership
  release point.

## Affected Subsystems

- Storage nested statement checkpoint cleanup and reuse.
- Prepared row-DML transaction hot path.

## Compatibility Impact

No SQL, C API, storage-engine routing, metadata, or file-format behavior
changes. This only narrows process-local cleanup before a nested checkpoint
object is retained for reuse.

## Single-File And Embedded Lifecycle

No durable file or companion-file lifecycle change. Journal ownership flags,
deferred durable-cache retarget state, and rollback preservation flags are
explicitly cleared before reuse.

## Binary Size, License, And Dependencies

No new dependency and no upstream import.

## Test And Verification Plan

- Build `mylite_storage_test`, `mylite_embedded_statement_test`,
  `mylite_embedded_storage_engine_test`, and `mylite_perf_baseline`.
- Run focused storage and embedded smoke tests.
- Run full `storage-smoke-dev` CTest.
- Run `git diff --check` and `git clang-format --diff`.
- Run prepared-update performance baseline and sample a long run.

## Acceptance Criteria

- Reusable nested checkpoint cleanup no longer calls the full fresh nested
  initializer.
- Fresh nested checkpoint allocations still start from a clean state.
- Existing transaction, savepoint, rollback, and embedded storage-engine tests
  pass.
- Prepared-update profiling moves `initialize_nested_checkpoint_storage()` down
  or removes it from the nested checkpoint cleanup path.

## Risks And Open Questions

- This relies on `free_statement()` remaining the only path that stores a
  reusable nested checkpoint object and on its owned-state cleanup running
  before the object is cached. If new owned fields are added to
  `mylite_storage_statement`, the cleanup path must be reviewed with this
  helper.

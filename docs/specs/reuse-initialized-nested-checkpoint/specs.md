# Reuse Initialized Nested Checkpoint

## Problem Statement

Prepared row-DML samples still show `initialize_nested_checkpoint_storage()` in
the hot loop. Nested checkpoint objects are already reset before they are stored
in the thread-local reusable slot, but allocation immediately resets the same
object again when it is taken back out.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite first-party storage owns nested statement checkpoints in
  `packages/mylite-storage/src/storage.c`.
- `free_statement()` calls `initialize_nested_checkpoint_storage()` before
  saving a reusable nested checkpoint object in
  `reusable_nested_checkpoint_statement`.
- `allocate_checkpoint_statement()` then calls the same initializer again for
  both reused and freshly allocated nested checkpoint objects.

## Proposed Design

- When `allocate_checkpoint_statement()` takes an object from
  `reusable_nested_checkpoint_statement`, return it directly because it was
  already reset before caching.
- Keep the existing initializer for freshly allocated nested checkpoint objects.
- Keep top-level checkpoint allocation on `calloc()`.

## Affected Subsystems

- Storage nested statement checkpoint allocation and cleanup.
- Prepared row-DML transaction hot path.

## Compatibility Impact

No SQL, C API, storage-engine routing, metadata, or file-format behavior
changes. This only removes redundant initialization of an already-reset
first-party storage object.

## Single-File And Embedded Lifecycle

No durable file or companion-file lifecycle change. Nested statement checkpoint
ownership and rollback behavior remain unchanged.

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

- Reused nested checkpoint objects are not initialized twice per statement.
- Fresh nested checkpoint allocations still start from a clean state.
- Existing transaction, rollback, and embedded storage-engine tests pass.
- Prepared-update profiling moves `initialize_nested_checkpoint_storage()` down
  or exposes the next dominant storage-owned cost.

## Risks And Open Questions

- The change relies on `free_statement()` being the only path that stores a
  reusable nested checkpoint object, and that path already performs the reset.

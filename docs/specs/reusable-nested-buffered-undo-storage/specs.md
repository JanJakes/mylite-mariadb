# Reusable Nested Buffered Undo Storage

## Problem

Prepared row-DML statements inside an explicit transaction repeatedly allocate,
release, and re-adopt the same small buffered-page undo array. Each nested
statement needs a fresh undo *count*, but the common row-only update path only
captures one compact row-page preimage before the reusable nested checkpoint is
returned to the thread-local statement cache.

The sampled prepared-update profile after fixed exact-index hashing still shows
`capture_buffered_page_undo_from_page()` and reusable undo adoption under the
active buffered rewrite path.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), with this slice limited to
  first-party MyLite storage code.
- `free_statement()` already keeps one nested checkpoint object in
  `reusable_nested_checkpoint_statement` when the nested statement borrows its
  parent file and filename.
- `clear_buffered_page_undos()` releases the nested statement's small undo
  entry array to a separate thread-local `reusable_buffered_page_undos` cache.
- The next nested statement then calls `adopt_reusable_buffered_page_undos()`
  during first undo capture to move that same array back to the statement.
- Buffered undo buckets are only useful for larger undo lists and are explicitly
  not reused by the existing small-list cache.

## Design

- When a nested checkpoint object itself is being retained for reuse, reset its
  small buffered-page undo list in place instead of releasing it to the separate
  reusable undo cache.
- Keep only bounded small undo arrays whose capacity is at or below
  `MYLITE_STORAGE_REUSABLE_BUFFERED_PAGE_UNDO_CAPACITY`.
- Reset `count` to zero and drop bucket state so no rollback preimage survives
  into the next statement.
- Keep the existing release path for ordinary statements, oversized undo lists,
  and cases where the reusable nested checkpoint slot is already occupied.

## Compatibility Impact

No SQL, public C API, storage-engine routing, or file-format behavior changes.
The slice only changes transient allocation ownership for statement rollback
preimage arrays after the statement has committed or rolled back.

## Single-File And Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle changes.
Rollback semantics are preserved because reusable undo lists are reset to zero
entries before the nested statement object is cached.

## Tests And Verification

- Run:
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-updates 1000 1000000`
  - sampled prepared-update run to confirm reusable undo adoption drops out of
    the active rewrite hot path
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`

## Acceptance Criteria

- The reusable nested checkpoint keeps a bounded buffered-page undo entry array
  with zero entries between executions.
- Rollback and savepoint coverage remains green.
- Oversized or non-reused undo lists still free or move through the existing
  release path.
- Prepared-update performance is neutral or improved.

## Risks

- Stale undo entries must not survive statement reuse. The implementation
  resets the entry count and drops bucket state before caching the nested
  statement object.

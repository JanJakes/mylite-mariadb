# Cached Rewrite State Page Skip

## Problem Statement

Prepared primary-key updates still spend most storage-side samples inside
`rewrite_active_update_pages()`. The cached-shape path has already validated a
row's active rewrite layout, but still resolves the buffered row-state page on
every later rewrite even when it will not decode that page.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite first-party storage code owns active append-buffer rewrites in
  `packages/mylite-storage/src/storage.c`.
- `buffered_append_page_range_contains_in_statement()` proves the row page,
  state page, and changed index-entry pages are inside the contiguous active
  append buffer before cached-shape handling starts.
- `buffered_update_rewrite_shape_known()` means the row-state page and changed
  index-entry page shape was already decoded successfully for that row.
- The row-state page pointer is only needed when the shape is not cached and
  the function must decode or validate row-state metadata.

## Proposed Design

- Keep the existing contiguous range check for every active rewrite.
- Resolve the row-state page only when `use_cached_shape` is false.
- Preserve all uncached validation and fallback behavior.

## Affected Subsystems

- Active append-buffer update rewrite hot path.
- Prepared-update performance baseline.

## Compatibility Impact

No SQL, C API, storage-engine routing, metadata, hash distribution, or
file-format behavior changes. Cached-shape rewrites still require the same
previous successful validation and contiguous buffered page range.

## Single-File And Embedded Lifecycle

No durable file or companion-file lifecycle change.

## Binary Size, License, And Dependencies

No new dependency.

## Test And Verification Plan

- Build `mylite_storage_test`, `mylite_embedded_statement_test`,
  `mylite_embedded_storage_engine_test`, and `mylite_perf_baseline`.
- Run focused storage and embedded smoke tests.
- Run full `storage-smoke-dev` CTest.
- Run `git diff --check` and `git clang-format --diff`.
- Run prepared-update performance baseline and sample a long run.

## Acceptance Criteria

- Cached active rewrite shape no longer performs a row-state page lookup on
  every hot rewrite.
- Uncached rewrite validation continues to decode row-state metadata.
- Storage and embedded tests pass.
- Prepared-update profiling reduces `rewrite_active_update_pages()` samples or
  shows the next bottleneck clearly.

## Risks And Open Questions

- This relies on the append buffer remaining contiguous for the already-checked
  rewrite range. That is the current buffer invariant and is already required
  by the index-page rewrite path.

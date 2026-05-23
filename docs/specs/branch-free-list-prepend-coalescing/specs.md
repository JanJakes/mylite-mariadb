# Branch Free-List Prepend Coalescing

## Problem

Branch final-child reclamation currently publishes every removed leaf as an
independent one-page free-list run. Repeated final-child removals commonly
remove descending adjacent leaf pages, so the free-list grows as multiple small
nodes even though those pages form one reusable contiguous run.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are needed.
- MyLite free-list pages are root pages for reusable runs. Each root records a
  `run_start_page`, `run_page_count`, and `next_root_page`.
- Final-child branch reclamation already owns the removed leaf page and journals
  its preimage before overwriting it as a free-list page.
- If the removed leaf is immediately before the current free-list root run, the
  removed leaf can become the new root for an expanded run by copying the old
  root's `next_root_page` and increasing the run length. The old root page does
  not need to be modified.

## Scope

- Coalesce a reclaimed branch leaf with the current free-list root only when
  `leaf_page_id + 1 == current_root.run_start_page`.
- Encode the removed leaf as the new root of the larger contiguous run.
- Preserve the current root's `next_root_page` so the free-list chain remains
  intact.
- Keep non-adjacent reclamation on the existing one-page prepend path.

## Non-Goals

- No general free-list search or best-fit allocator.
- No coalescing with a following page above the current root run.
- No coalescing across arbitrary later free-list nodes.
- No file shrinking.
- No row or index page allocation from the free-list.

## Compatibility Impact

No SQL, public API, or storage-engine routing behavior changes. This only
changes internal free-list shape after branch leaf reclamation.

## Single-File And Lifecycle Impact

All reclaimed pages remain in the primary `.mylite` file. Rollback and stale
journal recovery still restore the old branch leaf page and previous header.

## Recovery Impact

The removed leaf is already protected by the statement or transaction journal.
The existing free-list root page is only read, not modified, for lower-adjacent
prepend coalescing, so existing recovery coverage remains sufficient.

## Test Plan

- Add controlled storage-hook coverage for the reclaim helper to assert an
  adjacent removed leaf becomes a larger free-list run.
- Keep branch final-child removal coverage asserting non-adjacent reclamation
  stays on the one-page prepend path.
- Keep rollback and stale-journal recovery assertions verifying the previous
  single-page root is restored before the second removal commits.
- Run the storage and storage-smoke verification set.

## Acceptance Criteria

- Reclaiming a leaf immediately before the current root run publishes a single
  larger free-list run rooted at the newly reclaimed leaf.
- Non-adjacent reclamation still publishes an independent one-page run.
- Rollback and stale recovery restore the previous free-list root and branch
  leaf bytes.
- Docs and roadmap distinguish prepend coalescing from broader free-list
  coalescing and allocation work.

## Implementation Notes

- Branch leaf reclamation now reads the current free-list root before encoding
  the reclaimed leaf.
- If the reclaimed leaf immediately precedes the current root run, the
  reclaimed leaf becomes the new root of the expanded run and preserves the old
  root's `next_root_page`.
- The old root page is not rewritten; it becomes part of the reusable run. This
  keeps the existing recovery journal coverage sufficient because the only
  newly dirtied page is still the removed branch leaf.
- A storage test hook exercises the internal reclaim helper against a controlled
  adjacent free-list root. The branch final-child removal tests continue to
  cover the public non-adjacent path, rollback, and stale recovery.

## Verification Results

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`

## Risks And Follow-Ups

- Coalescing remains intentionally local to the head of the free-list chain.
- Reclaimed row/index pages are still not allocated from the free-list.

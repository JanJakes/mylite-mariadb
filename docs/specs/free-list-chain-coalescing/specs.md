# Free-List Chain Coalescing

## Problem

Catalog and branch-leaf reclamation currently coalesce a reclaimed page run only
with the current free-list root run. If the adjacent free run is deeper in the
free-list chain, MyLite publishes another one-run node instead of merging the
runs. That preserves correctness, but fragments reusable space and leaves more
free-list nodes for future allocation work.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This slice is first-party MyLite storage work. MariaDB storage engines have
  their own page and extent free-space management, but they are not the source
  authority for MyLite's single-file page format.
- MyLite stores free runs as `MYLFRE1` pages, one root page per run, linked by
  `next_root_page`.
- `encode_reclaimed_free_list_root_page()` only checks adjacency against
  `header->free_list_root_page`.
- `reclaim_catalog_page_run()` must start a recovery journal before rewriting
  existing free-list pages. Branch-leaf reclamation runs through the pager, but
  its delete plan still needs to protect existing free-list pages before row
  append creates the statement journal.

## Scope

- Plan reclaimed free-list runs by scanning the current free-list chain.
- Merge a reclaimed run with any adjacent previous or next free run in the
  chain, including bridge cases where the reclaimed run connects two existing
  free runs.
- Reject overlapping free-list runs as corruption instead of publishing a
  double-free chain.
- Keep the bounded write shape to at most two free-list pages:
  - an existing before-run page, optionally plus the previous node that pointed
    at an after-run page; or
  - a new reclaimed-run page, optionally plus the previous node that pointed at
    an after-run page.
- Expose the existing free-list pages that will be rewritten so catalog and
  branch reclamation can protect them in the recovery journal.

## Non-Goals

- No sorted free-list chain invariant.
- No free-list allocation from non-root runs.
- No file truncation or durable page relocation.
- No page-format change.

## Compatibility Impact

No SQL-visible behavior changes. The slice only changes how reusable pages are
linked inside the `.mylite` file.

## Single-File And Lifecycle Impact

All durable state remains inside the primary `.mylite` file. Active statement
rollback and stale journal recovery must restore any existing free-list page
whose `next_root_page` or run size changes during chain coalescing.

## File-Format Impact

No file-format change. Existing free-list pages are rewritten with the current
`MYLFRE1` format.

## Test Plan

- Add catalog free-list tests for non-root prepend, non-root append, and bridge
  coalescing.
- Cover active-statement rollback for a coalesce that rewrites a non-root
  predecessor pointer.
- Add branch-leaf reclamation coverage for non-root chain coalescing through the
  storage test hook.
- Preserve existing root prepend/append coalescing, free-list reuse, corrupt
  free-list, and storage-smoke coverage.
- Run:

```sh
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test
ctest --preset storage-smoke-dev --output-on-failure
```

## Acceptance Criteria

- Reclaimed catalog and branch-leaf runs coalesce with adjacent free-list runs
  beyond the current root.
- Bridge reclaim merges two adjacent free-list runs into one linked run.
- Active statement rollback restores rewritten free-list predecessor/root pages.
- Overlapping reclaimed/free-list runs are rejected as corruption.
- Existing root-adjacent coalescing and free-list reuse behavior remain intact.

## Risks And Follow-Ups

- Allocation still reuses only the root run. Reusing suitable non-root runs
  remains separate follow-up work.
- The chain remains recency-ordered rather than sorted by page id.
- Broader file shrinking and free-space compaction remain planned.

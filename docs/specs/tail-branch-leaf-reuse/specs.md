# Tail Branch Leaf Reuse

## Problem

Branch child-count-reducing deletes can reclaim a removed leaf page into the
durable free list. When the removed leaf is the physical final page, the same
delete still appends a row-state page after the old file end, so the file grows
by one page while also publishing a reusable tail page. This is correct, but it
misses an immediate no-format-change reuse opportunity.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc::handler::ha_delete_row()` dispatches row deletion
  to the storage engine and leaves physical storage layout to that engine.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::delete_row()` routes durable
  deletes to `mylite_storage_delete_row()`.
- `packages/mylite-storage/src/storage.c::mylite_storage_delete_row()` writes
  maintained index-root changes before appending the delete row-state page.
- `packages/mylite-storage/src/storage.c::reclaim_removed_branch_leaf_page()`
  publishes reclaimed branch leaves through the durable free-list chain.

## Design

- Add a delete-specific tail reclaim helper used only by branch delete writers.
- If the removed branch leaf is exactly `header.page_count - 1`, lower the
  in-memory header `page_count` to that page id and skip free-list publication.
- Let the existing delete row-state append use the lowered `page_count`, so the
  row-state page overwrites the removed leaf page and the final published
  `page_count` returns to the pre-delete value.
- Keep generic free-list reclaim behavior and test hooks unchanged for
  non-delete callers and non-tail pages.
- Keep rollback and stale-journal recovery under the existing protected-page
  journal: the removed leaf page is already a protected dirty page and the
  saved header restores the old page count before truncation.

## Non-Goals

- No general file shrinking for row pages, row-state pages, catalog chains,
  BLOB pages, or interior free-list runs.
- No free-list tail trimming when no row-state append follows.
- No compaction or page relocation.

## Compatibility Impact

SQL-visible delete behavior does not change. This only changes the physical
page chosen for the delete row-state in a branch-maintained index shape.

## Single-File And Lifecycle Impact

Durable state remains in the primary `.mylite` file. The slice avoids creating
a durable free-list node for a tail page that the same delete immediately
reuses. Existing statement and transaction journals remain the only transient
sidecars.

## Public API, Storage Routing, And Wire Protocol

No public `libmylite` API, storage-engine routing, SQL policy, or wire-protocol
change.

## Binary Size And Dependencies

One small storage helper. No dependency or meaningful binary-size impact.

## Tests And Verification

- Update branch delete tests that remove the physical final branch leaf to
  assert unchanged final `page_count`, unchanged free-list root, and rollback or
  stale-journal recovery.
- Keep existing generic branch free-list reclaim tests unchanged so non-delete
  reclaim still publishes durable free-list runs.
- Run focused storage tests, storage smoke, whitespace checks, and clang-format
  diff checks.

## Acceptance Criteria

- Tail branch-leaf removal followed by delete row-state publication no longer
  grows the file by one page.
- Non-tail branch-leaf removal still publishes durable free-list runs.
- Generic free-list reclaim hooks keep their existing coalescing behavior.
- Rollback and stale-journal recovery restore the original branch leaf and file
  header.

## Risks

- This is a reuse slice, not full compaction. Most deleted data pages still
  remain in-place until broader free-space and file-shrinking work lands.

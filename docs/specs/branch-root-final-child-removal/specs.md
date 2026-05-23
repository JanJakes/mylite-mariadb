# Branch Root Final Child Removal

## Problem

Single-level branch roots can now maintain final-child inserts, final-leaf
deletes that keep the same child count, and final-leaf updates that stay in the
same final child. Deleting the only remaining entry in the final child still
falls back to the row-state overlay because the branch child list must shrink.
That keeps stale page-owned branch counts around and leaves later final-child
mutations with less accurate physical structure than necessary.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are needed.
- `plan_branch_index_root_delete()` currently plans a physical final-child
  delete only when decrementing the branch entry count preserves the existing
  child count.
- `delete_branch_index_leaf_entry()` rewrites the final leaf and refreshes the
  branch fence, but it does not remove branch child cells.
- Branch-root readers use recorded child page ids and scan only pages after the
  highest referenced child page as the append-tail visibility overlay. An
  unreferenced old final leaf can therefore remain as a durable skip page until
  later free-list or compaction work.

## Scope

- Plan a branch-root final-child removal only when:
  - the root is a single-level branch root with a page-owned nonzero entry
    count;
  - deleting the source row from the final child reduces the expected child
    count by exactly one;
  - the source row is in the final child leaf and that leaf has exactly one
    entry; and
  - at least one branch child remains after removal.
- Rewrite the branch root with the final child cell removed and the branch
  entry count decremented.
- Keep the deleted final leaf page durable but unreferenced. It remains an
  index leaf skip page for tail-overlay scans until a later reclamation slice.
- Preserve the normal row-state delete overlay for logical visibility and
  recovery.

## Non-Goals

- No branch-root collapse back to a single-page maintained root.
- No file shrinking, branch leaf reclamation, free-list publication, or page
  reuse.
- No interior child removal or redistribution.
- No multi-level branch roots.

## Compatibility Impact

No SQL, public API, or storage-engine routing behavior changes. The change only
keeps the physical branch-root snapshot closer to the live row set for a
supported delete shape.

## Single-File And Lifecycle Impact

All durable state remains inside the primary `.mylite` file. The branch root is
the only dirty existing page required for the removal path; the old final leaf
page is not modified and remains harmless unreachable storage.

## Recovery Impact

Rollback and stale-journal recovery must restore the branch root child cell and
entry count when a maintained final-child removal does not commit. The row-state
delete page remains append-only and is truncated on rollback with the rest of
the statement tail.

## Test Plan

- Extend branch-root storage coverage after the final-leaf update slice so a
  final child contains exactly one row.
- Cover statement rollback plus stale statement and transaction recovery for a
  final-child removal.
- Commit one final-child removal and verify branch entry count, child count,
  final branch fence, exact lookup, full index reads, row materialization, and
  later final-child delete/update behavior over the shrunken branch root.
- Run the storage and storage-smoke verification set.

## Acceptance Criteria

- Eligible final-child removal rewrites only the branch root and preserves the
  branch-root reader invariant.
- Unsupported delete shapes keep the row-state overlay path.
- Rollback and stale recovery restore the branch root bytes and logical
  visibility.
- Docs and roadmap describe final-child removal separately from future page
  reclamation and branch-root collapse work.

## Implementation Notes

- Delete planning now distinguishes stable final-leaf deletes from deletes that
  reduce the expected branch child count by one.
- Final-child removal plans protect the branch root before writing the row-state
  delete page. The removed leaf is validated but not dirtied.
- The writer removes the final branch cell, decrements the branch entry count,
  rewrites the child count and used bytes, and leaves the old final leaf as an
  unreferenced index leaf page.
- The existing row-state delete overlay remains the logical visibility record
  and rollback target.

## Verification Results

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`

## Risks And Follow-Ups

- The unreferenced final leaf is durable dead space until branch leaf
  reclamation/free-list work lands.
- Branch roots with one remaining child are still branch roots; collapsing them
  back to a single-page maintained root remains a separate compaction slice.

# Branch Arbitrary Child Removals

## Problem

Single-level branch roots can remove the final child leaf when deleting its only
entry reduces the expected branch child count. Equivalent one-entry interior
children still fall back to row-state overlay even when removing the child cell
would leave a valid branch root.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are needed.
- `plan_branch_index_root_delete()` currently scans only the final child when a
  delete would reduce the expected child count.
- `remove_branch_index_final_child_entry()` removes the final branch cell and
  reclaims the leaf page; the same branch-cell rewrite can remove an arbitrary
  child by shifting later cells left.
- Branch readers use child cells in branch order and follow stored child page
  ids, so removing an interior child does not require physically contiguous leaf
  page ids.

## Scope

- Plan a physical child removal for any child leaf when:
  - the branch is a single-level branch root;
  - deleting the row reduces the expected child count by one;
  - the target child leaf contains exactly the deleted row;
  - the remaining children preserve branch order and page-count invariants.
- Rewrite the branch root without the removed child cell.
- Reclaim the removed leaf page through the existing durable free-list path.
- Preserve maintained-root collapse when the remaining live entryset fits one
  maintained root page.

## Non-Goals

- No merge, borrow, or redistribution when the expected child count stays the
  same.
- No branch-page-full split/merge or multi-level branch tree.
- No arbitrary free-list chain coalescing beyond the existing reclaim path.
- No SQL, public API, storage-engine routing, or file-format change.

## Compatibility Impact

No SQL-visible behavior changes. Eligible deletes avoid append-tail row-state
index overlay for more single-level branch-root shapes.

## Single-File And Lifecycle Impact

All durable changes stay in the primary `.mylite` file. Statement rollback,
transaction rollback, and stale-journal recovery restore the protected branch
root and removed leaf page. Header recovery restores the previous free-list root
when a removal is not committed.

## File-Format Impact

No format-version change. The slice uses existing branch-root, leaf, free-list,
and journal page formats.

## Test Plan

- Extend branch-root storage coverage with deletion of the only entry in an
  interior child after a branch-ordered interior split.
- Assert the delete is visible inside a statement, removes one child cell,
  publishes the removed leaf to the free list, preserves full index read order,
  and rolls back.
- Cover a same-statement split followed by removal of the newly appended child,
  where rollback truncates the post-snapshot child page instead of needing a
  saved preimage.
- Cover stale statement and transaction recovery for the same removal path.
- Preserve same-child delete, final-child removal, collapse, split, refold, and
  update coverage.
- Run the storage and storage-smoke verification set.

## Acceptance Criteria

- Eligible arbitrary child removals rewrite the branch root without the removed
  child and reclaim that leaf page.
- Branch exact lookup, indexed row lookup, and full index reads remain correct
  after the removal.
- Rollback and stale recovery restore the previous branch root, removed leaf,
  free-list root, and logical visibility.
- Existing rollback journals treat protected pages appended after the saved
  header as truncate-owned pages rather than corrupt preimage requests.
- The roadmap distinguishes this from broader merge, borrow, redistribution,
  branch-page split, and multi-level navigation work.

## Risks And Follow-Ups

- This still leaves underfull-child merge/redistribution when child count does
  not decrease.
- Physical leaf page ids may remain non-monotonic after earlier interior splits;
  future branch maintenance must keep using branch child cells as the source of
  truth.

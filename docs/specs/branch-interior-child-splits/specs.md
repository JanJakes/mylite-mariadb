# Branch Interior Child Splits

## Problem

Single-level branch roots can split a full final child leaf when the branch has
room for another child and there is no live append-tail overlay to hide. Full
interior child leaves still fall back to append-tail overlay even though the
existing split writer can encode the split child in branch order.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are needed.
- `plan_branch_index_root_insert()` currently plans physical splits only for a
  full final child.
- `split_branch_index_leaf_entry()` rejects non-final split children, but
  `copy_index_branch_children_with_split()` already inserts the replacement
  first leaf and appended second leaf at the requested branch child offset.
- Branch readers follow branch child page ids and high `(key, row_id)` fences,
  so branch-order child cells do not require physically contiguous or
  monotonically increasing leaf page ids.

## Scope

- Plan a physical split for any full existing child leaf when:
  - the branch is a single-level branch root;
  - the branch is at packed capacity before the insert;
  - the branch has child-cell capacity for one more child;
  - no live row-state or index-entry overlay exists after the current branch
    child pages;
  - the inserted entry routes to the full child being split.
- Reuse the existing split writer to rewrite the original child leaf, append
  one new leaf, and rewrite the branch root with the new child cell inserted in
  branch order.
- Preserve the existing refold path when slack already exists in the branch.

## Non-Goals

- No branch split when the branch page itself is full.
- No multi-level branch tree.
- No merge, borrow, or redistribution.
- No split that hides an existing live append-tail overlay.
- No SQL, public API, storage-engine routing, or file-format change.

## Compatibility Impact

No SQL-visible behavior changes. Eligible inserts avoid append-tail index-entry
fallback for more single-level branch-root shapes.

## Single-File And Lifecycle Impact

All durable changes stay in the primary `.mylite` file. Statement rollback,
transaction rollback, and stale-journal recovery restore the protected branch
root and source child leaf; the appended row and new leaf are truncated by
header recovery.

## File-Format Impact

No format-version change. The slice uses the existing branch-root, leaf, and
journal page formats.

## Test Plan

- Extend branch-root storage coverage with an insert that targets a full
  interior child and splits it into two branch-ordered child cells.
- Assert the insert is visible inside a statement, does not append a fallback
  index-entry page, preserves full index read order, and rolls back.
- Cover stale statement and transaction recovery for the same interior split.
- Preserve final-child split, refold, same-child update/delete, cross-child
  update, final-child delete/removal, and free-list coverage.
- Run the storage and storage-smoke verification set.

## Acceptance Criteria

- Eligible interior-child inserts split the child leaf and rewrite the branch
  root without append-tail index-entry fallback.
- Branch exact lookup, indexed row lookup, and full index reads find entries in
  branch order after the split.
- Rollback and stale recovery restore the previous branch root and source leaf
  and remove the inserted row and appended new leaf.
- The roadmap distinguishes this bounded split from branch-page splits,
  multi-level navigation, merge, borrow, and redistribution.

## Risks And Follow-Ups

- This still leaves branch-page-full splits and multi-level B-tree navigation
  for future work.
- Interior splits make physical leaf page ids non-monotonic relative to branch
  order, so future code must keep relying on branch child ids and fences.

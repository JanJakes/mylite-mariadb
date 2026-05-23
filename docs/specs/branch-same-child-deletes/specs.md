# Branch Same-Child Deletes

## Problem

Branch-root deletes currently physically rewrite only the final child leaf. A
delete from an interior child leaf falls back to append-tail row-state overlay
even when the source leaf can remain non-empty and the branch fence can be
refreshed safely under the existing root-plus-leaf journal protection.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are needed.
- MyLite single-level branch roots route reads through child high fences and
  stored child page ids.
- Existing branch delete journaling already protects one branch root and one
  leaf page before physical final-child leaf deletes.
- Existing branch fence refresh code can update the child fence after a leaf
  loses an entry when the branch child count stays stable.
- Existing branch insert refolding can rebuild a single-level branch into fresh
  packed leaves when a target child is full but the branch as a whole still has
  capacity.

## Scope

- Plan branch deletes for any child leaf that contains the source row when the
  post-delete child count remains unchanged.
- Keep the physical rewrite limited to leaves with more than one entry before
  the delete so the child remains non-empty.
- Preserve the existing final-child removal path when deleting the sole final
  child entry reduces the expected branch child count.
- Let branch reads validate branch-backed child leaves by actual non-empty
  occupancy rather than fixed packed-run occupancy.
- Refold a branch insert when a same-child delete leaves slack in one child but
  a later insert targets a different full child.
- Continue using append-tail row-state overlay for deletes that would require
  borrowing, merging, or removing an interior child.

## Non-Goals

- No branch merge, borrow, redistribution, or multi-level B-tree changes.
- No interior child removal.
- No file shrinking or broader free-list allocation changes.
- No SQL or public API behavior change.

## Compatibility Impact

No SQL, public API, or storage-engine routing behavior changes. Supported
deletes avoid append-tail index visibility work for more branch child leaves.

## Single-File And Lifecycle Impact

The rewrite remains inside the primary `.mylite` file. Statement rollback,
transaction rollback, and stale-journal recovery restore the protected root and
leaf bytes.

## Test Plan

- Extend branch-root storage coverage with an interior child delete that leaves
  the leaf non-empty.
- Assert the delete is visible inside a statement, does not append a fallback
  index-entry page, refreshes the branch entry count, and rolls back.
- Assert full branch entry reads still work while a non-tail child is underfull.
- Cover stale statement and transaction recovery for the same path.
- Preserve existing high-key insert refold coverage after an earlier physical
  same-child delete in the same statement.
- Run the storage and storage-smoke verification set.

## Acceptance Criteria

- Interior same-child deletes rewrite the source leaf and branch fence without
  append-tail index-entry fallback.
- Branch leaf-run readers accept non-empty underfull branch children and still
  reject inconsistent total branch entry counts.
- Inserts after an underfull-child delete refold when the target child is full
  but the branch still has capacity.
- Deletes that would empty an interior child remain on the existing fallback
  path.
- Rollback and stale recovery restore the prior branch root, leaf, and logical
  visibility.
- Docs distinguish same-child deletes from future branch merge/borrow work.

## Risks And Follow-Ups

- Interior children may be less than full after this slice; branch lookup uses
  fences rather than fixed occupancy, but merge/redistribution remains future
  work.
- Cross-child redistribution and full B-tree delete balancing remain planned.

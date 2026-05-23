# Branch Cross-Child Updates

## Problem

Branch-root updates now rewrite a child leaf when the replacement `(key, row_id)`
stays in the same child range. Updates that move an index entry to another
existing child still fall back to append-tail overlay even when the source child
can remain non-empty and the destination child has room.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are needed.
- Single-level branch roots store child page ids and high `(key, row_id)`
  fences. A replacement tuple can be routed to an existing target child through
  the branch fence search.
- Existing same-child update code already rewrites one leaf and refreshes one
  branch fence under root-plus-leaf journal protection.
- The recovery journal supports enough protected pages for root-plus-two-leaf
  updates.

## Scope

- Plan a branch update that moves an entry between two existing child leaves
  when:
  - the source leaf contains the source row and has more than one entry;
  - the target leaf is distinct and has room for one more entry;
  - the replacement tuple routes to the target child by current branch fences;
  - the branch child count remains stable.
- Rewrite the source leaf without the source row and the target leaf with the
  replacement entry.
- Refresh both affected branch child fences.
- Protect the root page, source leaf page, and target leaf page before writing.

## Non-Goals

- No split, merge, borrow, or redistribution.
- No movement into a full target child.
- No movement that would empty the source child.
- No multi-level branch-tree support.
- No SQL, public API, or storage-engine routing change.

## Compatibility Impact

No SQL-visible behavior changes. Supported updates avoid append-tail index
overlay pages for more single-level branch-root updates.

## Single-File And Lifecycle Impact

All durable changes stay in the primary `.mylite` file. Statement rollback,
transaction rollback, and stale-journal recovery restore the protected root and
both leaf pages.

## File-Format Impact

No format-version change. The slice uses existing branch-root, leaf, and journal
page formats.

## Test Plan

- Extend branch-root storage coverage with an update that moves an interior
  child entry into another existing child with free capacity.
- Assert the update is visible inside a statement, does not append a fallback
  index-entry page, refreshes full index reads, and rolls back.
- Cover stale statement and transaction recovery for the same cross-child path.
- Preserve same-child, final-child, split, refold, and removal coverage.
- Run the storage and storage-smoke verification set.

## Acceptance Criteria

- Eligible cross-child updates rewrite two leaves and the branch root without
  append-tail index-entry fallback.
- Ineligible cross-child updates with a full target or single-entry source
  remain on the existing fallback path.
- Rollback and stale recovery restore the previous branch root, source leaf,
  target leaf, and logical visibility.
- The roadmap distinguishes this from future split/merge/redistribution work.

## Risks And Follow-Ups

- This is still a single-level branch maintenance slice. Full B-tree
  rebalancing, split/merge, and multi-level navigation remain future work.
- Cross-child moves can make child occupancy less packed; branch readers must
  continue to rely on child ids and fences rather than fixed leaf-run counts.

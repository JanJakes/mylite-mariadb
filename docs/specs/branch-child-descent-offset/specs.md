# Branch Child Descent Offset

## Problem

Maintained branch insert planning descends through branch pages to choose a
child page, then often scans that same branch page again to recover the child
cell offset for sibling redistribution or fence validation. Local sampling of
the prepared-insert hot path after selected-leaf summary probing still showed
`find_index_branch_child_page()` and `find_index_branch_child_offset()` as
visible storage-owned planning costs.

## Source References

- MariaDB base: `mariadb-11.8.6` at
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- MyLite handler write path:
  `mariadb/storage/mylite/ha_mylite.cc::write_row()`.
- MyLite branch descent and planning:
  `packages/mylite-storage/src/storage.c::find_index_branch_child_page()`,
  `find_index_branch_child_page_for_insert()`,
  `plan_branch_index_root_insert()`,
  `plan_level_two_branch_index_root_insert()`,
  `plan_level_three_branch_index_root_insert()`,
  `plan_level_four_branch_index_root_insert()`, and
  `plan_deep_branch_index_root_insert()`.

## Design

Add branch-child descent helpers that return both the selected child page id
and the child cell offset from the same binary search. Insert-oriented descent
keeps the existing fallback to the final child when the lookup key is beyond
the branch fence range, and returns that final child offset with the page id.

Branch insert planning uses the combined helper where the offset is needed for
branch-child fence validation or leaf-range redistribution. Existing helpers
remain available for readers that only need the page id.

## Compatibility

No SQL, storage format, or public C API behavior changes. The selected child
page remains the same as the previous lookup-plus-offset sequence, including
the insert fallback to the final branch child.

## Tests

- `mylite_storage_test_find_index_branch_child_page_for_insert_offset()` checks
  exact child matches and final-child insert fallback offsets.
- Existing maintained branch insert, update, storage, and storage-engine smoke
  tests cover planner and writer behavior through the combined helper.

## Acceptance Criteria

- Branch insert planning no longer performs a second child-offset scan after a
  branch-child descent when the descent already knows the selected child.
- Insert fallback returns both the final child page id and final child index.
- Existing child-page-only lookup behavior remains intact for non-planning
  callers.

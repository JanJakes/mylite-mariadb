# Branch Leaf Split Before Refold

## Problem

Prepared insert performance is still dominated by single-level branch refolds
after branch leaf range planning was made one-pass. A fresh local sample of:

```sh
build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components --profile-iterations=200000 1000
```

shows the hot path under `write_branch_index_root_inserts()` ->
`refold_branch_index_root_insert()` -> `read_branch_index_root_entries()`,
with most time spent rereading leaf pages, checksumming them, rebuilding an
entryset, and writing a fresh branch snapshot.

The common shape is a full selected leaf in a single-level branch where the
branch still has child capacity but has spare entries somewhere outside the
nearby redistribution range. The current planner tries bounded range
redistribution, then refolds the whole branch snapshot if the full live entryset
still fits in one branch page. That preserves child count, but it is not the
natural B-tree mutation for a full target leaf.

## Goal

Plan a selected-leaf split before whole-root refold when a level-`1` branch:

- targets a full selected leaf,
- still has room for another child cell,
- has no live append-tail overlay that would be hidden by appending a new child
  leaf, and
- can split within the existing rollback-journal protected-page budget.

This should turn duplicate-heavy secondary-key prepared inserts from whole-root
refolds into localized leaf-plus-root rewrites when the branch has child
capacity.

## Non-Goals

- No durable page-format change.
- No general recursive B-tree split propagation.
- No change to multi-level branch insert planning.
- No new public `libmylite` API, handler API, or SQL behavior.
- No change to live-overlay safety rules.
- No removal of the existing bounded range redistribution or refold fallback.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- MariaDB insert execution still reaches MyLite storage through:
  - `mariadb/sql/sql_insert.cc:Write_record::single_insert()`
  - `mariadb/sql/handler.cc:handler::ha_write_row()`
  - `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()`
  - `mariadb/storage/mylite/ha_mylite.cc` calls
    `mylite_storage_append_row_with_index_entries()` for durable routed tables.
- MyLite storage currently has the needed local mutation primitive:
  - `packages/mylite-storage/src/storage.c:split_branch_index_leaf_entry()`
    can split the selected leaf, insert a new child cell at the selected child
    offset, and rewrite the branch root.
- The planner only chooses that primitive when
  `branch_page->entry_count == branch_page->child_count * leaf_capacity`, so
  branches with spare entries elsewhere can still fall through to refold.
- `index_branch_tail_has_live_overlay()` must still guard split planning because
  appending a new child leaf past live row-state or index-entry tail pages would
  move the branch subtree high page id past those tail pages.

## Compatibility Impact

No SQL compatibility claim changes. Rows inserted through routed `ENGINE=InnoDB`
and explicit MyLite tables keep the same key ordering, duplicate-key semantics,
rollback behavior, and handler write path. This is an internal storage planning
change that picks a more local mutation before the existing refold fallback.

## Design

In `plan_branch_index_root_insert()` for level-`1` roots, when the selected leaf
is full and the branch has child capacity:

1. Keep the existing bounded range redistribution attempt first when total
   branch entry capacity can absorb the new entry without adding a child.
2. If redistribution does not produce a plan, check whether the selected full
   leaf can be split locally before considering a full branch refold.
3. Reuse the existing no-live-overlay guard.
4. Append the same split-leaf plan currently used for fully packed branches.
5. Leave the refold path as fallback for live-overlay cases or oversized
   branch-child states.

The split writer already rereads the protected root and selected leaf, prepares
the two sorted leaf pages, inserts the new branch child cell, writes the old
leaf plus root through the maintained-insert pager, and appends one new leaf.

## File Lifecycle

Durable state remains in the primary `.mylite` file. The mutation appends one
new leaf page and rewrites the existing branch root and selected leaf under the
existing rollback-journal lifecycle. No new companion files are introduced.

## Embedded Lifecycle And API

No public embedded lifecycle or `libmylite` API behavior changes.

## Build, Size, And Dependencies

No dependency, license, generated artifact, or embedded build-profile change.
Binary-size impact should be limited to a small planner branch and focused test
coverage.

## Test Plan

- Add storage coverage for a single-level branch where the selected child leaf
  is full, the branch has child capacity, another child has spare entries, and
  no nearby redistribution range is chosen. Assert the insert appends one row
  page and one split leaf page, not a whole refolded leaf run.
- Preserve existing branch leaf range redistribution coverage.
- Preserve rollback coverage for statement rollback around the split.
- Run:

```sh
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_perf_baseline
build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
```

## Acceptance Criteria

- Eligible full selected leaves split before whole-root refold when the branch
  has child capacity and no live overlay.
- The existing refold fallback remains reachable when live overlay safety or
  branch capacity blocks the split.
- Existing single-level branch fitting insert, range redistribution, packed
  branch split, and rollback tests keep passing.
- Local prepared insert component timing records the new result or identifies
  the next measured bottleneck.

## Risks And Open Questions

- This may increase branch child count earlier than the prior minimal-child
  refold strategy. That is acceptable for this slice because it matches normal
  B-tree mutation and avoids repeated full-snapshot rewrites.
- The split still requires leaf decode/checksum work for the selected leaf and
  branch root; eliminating that is separate pager/cache work.
- Live row-state or index-entry overlays must continue to block the split
  because appending a new branch child would move branch-tail overlay bounds.

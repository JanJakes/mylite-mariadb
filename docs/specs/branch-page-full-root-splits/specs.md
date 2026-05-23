# Branch Page Full Root Splits

## Problem

Single-level branch roots can split a full child while the branch page still has
room for another child cell. When the branch page itself is full, supported
inserts fall back to append-tail index-entry pages even though the storage
reader can now navigate a bounded level-`2` branch root.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are needed.
- `plan_branch_index_root_insert()` currently handles full child leaves only
  when the single-level branch root still has child-cell capacity. When
  `branch_page->child_count >= index_branch_child_capacity()`, planning returns
  without suppressing the append-tail fallback.
- `split_branch_index_leaf_entry()` already rewrites the selected existing leaf
  and appends one new leaf with sorted entries.
- `copy_index_branch_children_with_split()` already produces the post-split
  leaf child order and high-key fences.
- `read_index_branch_leaf_run_root()` now accepts bounded multi-level branch
  roots and follows lower branch pages for exact, prefix, prefix-exists, and
  full-index reads.

## Scope

- Plan a root split for an insert into a full child of a packed full
  single-level branch root when:
  - the root is level `1`;
  - the target child leaf is full;
  - `child_count == index_branch_child_capacity(key_size)`;
  - `entry_count == child_count * leaf_capacity`;
  - the branch tail has no live row-state or same-index append-tail overlay;
  - the post-split leaf count fits the current bounded leaf-run child-id array;
  - the branch fanout can hold a level-`2` root with two lower branch children.
- Rewrite the existing root page as a level-`2` branch page.
- Rewrite the selected existing leaf, append one new leaf, and append two
  level-`1` branch pages that cover the post-split leaf child list.
- Keep page publication append-only until the header page count advances.
- Preserve fallback behavior for unsupported shapes.

## Non-Goals

- No arbitrary-depth B-tree split algorithm.
- No split when a live append-tail overlay would be hidden.
- No multi-level update/delete maintenance.
- No merge, borrow, redistribution, or free-list reclamation of superseded
  branch structure.
- No SQL-visible behavior, public API, storage-engine routing, or file-format
  change.

## Compatibility Impact

Supported exact, prefix, and full index reads continue to return the same
MariaDB-visible rows and ordering. The change only suppresses append-tail
fallback for a bounded fixed-width branch-root insert shape.

Unsupported multi-level row-DML shapes remain explicit fallbacks rather than
partial in-place mutations.

## Single-File And Lifecycle Impact

All durable state remains in the primary `.mylite` file. The existing root page
and selected child leaf are dirty pages protected by the statement or
transaction journal before mutation. The new leaf and lower branch pages are
appended beyond the saved header and become durable only when the header page
count is published. Rollback and stale-journal recovery restore the dirty
preimages and truncate the appended pages.

## File-Format Impact

No page format changes. This slice uses existing branch page `level`, child
page id, entry count, and high-key fence fields.

## Test Plan

- Add a storage unit test that uses a large fixed-width raw key to keep branch
  and leaf capacities small, fills a single-level branch root to branch-page
  capacity, and inserts one more row.
- Verify the root page becomes level `2`, has two lower branch children, and
  reports the incremented page-owned entry count.
- Verify exact lookup, prefix lookup, prefix-exists, indexed-row materialization,
  and full index reads over the new row.
- Verify no fallback index-entry page is needed for the split index by checking
  the expected page-count growth.
- Cover statement rollback, stale statement recovery, and stale transaction
  recovery for the root-split path.
- Keep existing single-level branch split/update/delete tests passing.
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

- An eligible insert into a packed full single-level branch root publishes a
  bounded level-`2` branch root instead of an append-tail index-entry fallback.
- The split root remains readable for exact, prefix, prefix-exists, indexed-row,
  and full-entryset storage paths.
- Statement rollback and stale statement/transaction journal recovery restore
  the previous single-level root and truncate appended split pages.
- Fallback behavior remains unchanged for live-overlay, unsupported fanout, and
  non-packed branch roots.

## Verification Results

Verified on 2026-05-23:

```sh
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test
ctest --preset storage-smoke-dev --output-on-failure
```

## Risks And Follow-Ups

- The bounded leaf-run representation still materializes leaf page ids; an
  unbounded production cursor should navigate branch pages without collecting
  every leaf.
- Future slices need multi-level branch insert/update/delete maintenance,
  branch-page merge/redistribution, and free-list reclamation for superseded
  pages.

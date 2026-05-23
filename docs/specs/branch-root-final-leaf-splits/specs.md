# Branch Root Final Leaf Splits

## Problem

Single-level branch roots now accept fitting in-range inserts and high-key
appends while the target leaf has spare capacity. Once the final child leaf is
full, append-heavy indexed inserts fall back to append-tail index-entry pages
even when the branch root still has room for another child. That keeps
correctness but reintroduces scan-overlay work on the common growing-key path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are needed.
- `read_index_branch_leaf_run_root()` and
  `read_index_leaf_run_page()` validate branch-rooted leaf runs with
  `index_leaf_run_expected_entry_count()`. That currently requires every
  non-final leaf to be full and only the final leaf to be partially filled.
- `plan_branch_index_root_insert()` can already select the final child for a
  high-key append beyond the branch high fence and suppress the fallback
  index-entry page after protected pages are planned.
- `insert_branch_index_leaf_entry()` already rewrites a dirty branch root plus
  one child leaf when the leaf has spare capacity.

## Scope

- When the selected child is the final child, the child leaf is full, and the
  branch root still has child capacity, split that final leaf into:
  - the existing leaf page, kept full; and
  - one newly appended leaf page containing the remaining sorted entries.
- Append the new leaf page after the row and any unrelated fallback index-entry
  pages, then advance the published header page count.
- Rewrite the branch root with one additional child cell, updated child fences,
  and an incremented page-owned entry count.
- Refuse the split when the existing branch tail contains row-state or
  same-index append-tail pages that would be hidden by moving the tail after the
  new leaf.
- Keep the existing append-tail fallback for non-final full leaves, branch
  roots without child capacity, unsupported key shapes, or validation failures.

## Non-Goals

- No interior leaf split or redistribution across later leaves.
- No multi-level branch roots.
- No branch merge or physical delete/update compaction.
- No change to branch-root reader invariants.

## Compatibility Impact

No SQL, public API, or storage-engine routing behavior changes. This changes
only the internal storage publication path for supported fixed-width indexes.

## Single-File And Lifecycle Impact

The branch root and original leaf are existing dirty pages protected by the
statement or transaction journal. The new leaf page is appended beyond the
current published header and becomes durable only when the header page count is
published. Rollback and stale-journal recovery must restore the old dirty pages
and truncate unpublished appended pages.

## Recovery Impact

Recovery coverage must prove the branch root and original full leaf are restored
after stale statement and transaction journals, and that the appended split leaf
does not survive without a header publish.

## Test Plan

- Extend branch-root storage coverage so a full final leaf split publishes one
  row page plus one new leaf page, without a fallback index-entry page for the
  split index.
- Verify root page-owned entry count, child count, final child id, final child
  fence, exact lookup, row materialization, and full index reads.
- Verify existing same-table tail overlay prevents the split and keeps recovery
  on the fallback path.
- Cover statement rollback, stale statement recovery, and stale transaction
  recovery for the split path.
- Keep fallback coverage for branch roots whose final child cannot split.
- Run:

```sh
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure
ctest --preset storage-smoke-dev --output-on-failure
```

## Acceptance Criteria

- A supported insert into a full final child leaf of a single-level branch root
  appends a new leaf and rewrites the branch root instead of publishing a
  fallback index-entry page.
- Branch-rooted readers still see a valid leaf run where every non-final leaf
  is full.
- Rollback and stale-journal recovery restore the pre-split root and leaf state.
- Non-final full leaves and full branch roots remain on the validated fallback
  path.

## Risks And Follow-Ups

- This is still not a general B-tree split algorithm. Interior splits require a
  reader-format change or redistribution across later leaves so sparse
  non-final leaves are not created.
- Split/merge/delete/update maintenance remains a follow-up after append-heavy
  growth no longer falls back immediately.

## Implementation Notes

- Branch insert planning now marks a branch operation as either an in-place leaf
  insert or a final-leaf split.
- Final-leaf splits are planned only when the target child is the last child,
  the leaf is full, the branch page has child capacity, and the branch entry
  count proves every current child leaf is full.
- The planner scans the existing branch tail before splitting. If it finds a
  row-state page for the table or an append-tail index-entry page for the same
  table/index, it leaves the insert on the fallback path so moving the branch
  tail cannot hide existing overlay state.
- The writer appends the new split leaf after the row and any unrelated
  fallback index-entry pages, rewrites the old final leaf as a still-full
  non-final leaf, and rewrites the branch page with one additional child cell.

## Verification Results

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`

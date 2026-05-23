# Branch Root High-Key Appends

## Problem

Branch-root inline inserts now handle keys that fall inside existing branch
fences, but a monotonically increasing key immediately beyond the branch high
fence still falls back to append-tail index-entry pages even when the last leaf
has spare capacity. Append-heavy primary-key and secondary-key workloads should
use the existing branch leaf until that leaf is full.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are needed.
- `find_index_branch_child_page()` returns `MYLITE_STORAGE_NOTFOUND` when the
  inserted `(key, row_id)` sorts beyond the current highest branch fence.
- `plan_branch_index_root_insert()` already validates the branch root and can
  preplan dirty branch and leaf pages for direct inserts.
- `refresh_index_branch_child_after_leaf_insert()` rewrites the child fence
  from the re-encoded leaf, so it can raise the final branch high fence after a
  direct high-key append.

## Scope

- When branch child lookup returns `MYLITE_STORAGE_NOTFOUND`, inspect the last
  child leaf of the single-level branch root.
- If the last child leaf has spare capacity, plan the insert against that leaf
  and suppress the fallback index-entry page.
- Reuse the existing direct branch-leaf rewrite path to update the leaf, branch
  page-owned entry count, and last child high fence.
- Keep append-tail fallback when the last child is full or branch/leaf
  validation fails.

## Non-Goals

- No branch split or child allocation.
- No multi-level branch tree.
- No update/delete branch maintenance.
- No append-tail compaction.

## Compatibility Impact

No SQL, public API, or storage-engine routing behavior changes. This only
changes the internal storage publication path for supported fixed-width indexes.

## Single-File And Lifecycle Impact

All modified pages are existing pages in the primary `.mylite` file. The same
statement or transaction dirty-page journal protects the branch root and last
leaf before either page is rewritten.

## Recovery Impact

Existing direct branch-insert rollback and recovery coverage should extend to a
high-key append because it dirties the same branch root and leaf pages while
also changing the last child fence.

## Test Plan

- Extend branch-root storage coverage so a key beyond the current branch high
  fence inserts directly when the last leaf has spare capacity.
- Verify the insert publishes only the row page, not a fallback index-entry
  page.
- Verify the branch page-owned entry count and last child fence advance.
- Verify exact lookup, full index reads, and row materialization see the
  inserted high-key row.
- Keep fallback coverage for full-leaf high-key inserts as future split work.
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

- A fitting high-key insert beyond a single-level branch root's current high
  fence rewrites the last leaf and branch root instead of writing a fallback
  index-entry page.
- The last branch child fence advances to the inserted high key.
- Existing fallback behavior remains for full last leaves.
- Recovery coverage remains valid for the dirty root and leaf pages.

## Risks And Follow-Ups

- This still stops at the first full last leaf. Splitting the leaf and adding a
  new branch child remains the next B-tree maintenance step.

## Implementation Notes

- Branch child lookup still handles in-range keys first.
- A `MYLITE_STORAGE_NOTFOUND` child lookup now selects the current final child
  as a high-key append candidate; the existing leaf-capacity check still decides
  whether to use direct branch maintenance or append-tail fallback.
- The existing direct branch-leaf rewrite updates the final child fence from the
  re-encoded leaf, so the branch high fence advances with the inserted key.

## Verification Results

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`

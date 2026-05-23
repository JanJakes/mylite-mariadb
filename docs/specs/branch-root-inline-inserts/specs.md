# Branch Root Inline Inserts

## Problem

Single-level branch roots now give fixed-width indexes a navigable static
snapshot, but row inserts after publication still write new index-entry pages to
the append tail. That is correct, but it grows the tail immediately after branch
publication and makes later lookups fold more page history than necessary.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are needed.
- `plan_maintained_index_root_inserts()` already runs before row append, marks
  handled index entries as unchanged for fallback index-entry publication, and
  preplans dirty pages for the statement or transaction journal.
- `decode_index_branch_page()` validates single-level branch roots, child page
  ids, child fences, and page-owned entry counts.
- `find_index_branch_child_page()` can choose the target child from branch
  high-key fences when the inserted `(key, row_id)` falls inside the current
  branch range.
- `decode_index_leaf_page()` and `encode_index_leaf_page()` already represent
  sorted immutable leaf-page contents, and `append_index_leaf_entries_to_entryset()`
  can fold leaf entries into a temporary entryset for re-encoding.

## Scope

- Extend insert planning to recognize single-level branch roots for matching
  fixed-width index entries.
- Predict the appended row id before the write journal is opened, so duplicate
  keys route to the same branch child that will contain the final `(key,
  row_id)` pair.
- If the selected child leaf has spare capacity, protect both the branch root
  and child leaf pages in the write journal, then rewrite the leaf with the new
  entry and rewrite the branch root with updated child fence and total entry
  count.
- Mark the handled index entry as unchanged so fallback index-entry pages are
  not written for that insert.
- Leave inserts that fall beyond the branch high fence, target a full leaf, or
  fail branch/leaf validation on the existing append-tail path.

## Non-Goals

- No branch split, merge, rebalance, or child allocation.
- No multi-level branch tree.
- No direct branch update or delete maintenance.
- No catalog rewrite.
- No attempt to compact existing append-tail entries.

## Compatibility Impact

No SQL, public API, or storage-engine routing behavior changes. This only
changes the internal publication path for supported fixed-width index inserts
that can fit into an existing single-level branch leaf.

## Single-File And Lifecycle Impact

All modified pages remain in the primary `.mylite` file. The existing dirty-page
journal protects the branch root and target leaf before either page is rewritten.
No companion files are introduced beyond the existing statement or transaction
journals.

## Recovery Impact

The slice must verify that statement rollback and stale statement/transaction
recovery restore both the rewritten leaf page and rewritten branch root when a
direct branch insert is abandoned.

## Test Plan

- Extend branch-root storage coverage to insert into a promoted branch root
  while the selected leaf has capacity.
- Verify no fallback index-entry page is written for the direct branch insert.
- Verify exact lookup, full index reads, and row materialization include the
  inserted entry after close/reopen.
- Verify branch root page-owned entry count and the selected child fence are
  updated.
- Verify inserts whose selected child is full or whose key is beyond the branch
  high fence still use the append-tail fallback.
- Add statement rollback plus stale statement and transaction recovery coverage
  for direct branch inserts.
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

- A fitting insert into a single-level branch root rewrites the target leaf and
  branch root instead of writing a fallback index-entry page.
- Branch root entry count and child fence bytes remain correct after the insert.
- Existing append-tail fallback remains intact for unsupported branch insert
  shapes.
- Rollback and recovery restore both dirty pages.

## Risks And Follow-Ups

- This is still not a full mutable B-tree. Full leaves continue to fall back to
  append-tail entries until split/merge slices exist.
- Delete and update maintenance remain append-tail overlays, so static branch
  entry counts remain page-owned snapshot counts rather than live counts.

## Implementation Notes

- Insert planning predicts the appended row page id before opening the write
  journal so duplicate-key routing uses the final `(key, row_id)` pair.
- Planned branch inserts protect both the branch root and target child leaf in
  the existing statement or transaction journal.
- The selected leaf is re-encoded from a temporary entryset with the new entry,
  and the branch page-owned entry count and child fence are refreshed from the
  rewritten leaf.
- The fallback index-entry page is suppressed only for planned branch inserts;
  unsupported shapes keep the append-tail path.

## Verification Results

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`

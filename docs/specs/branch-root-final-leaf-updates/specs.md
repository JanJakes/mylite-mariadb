# Branch Root Final Leaf Updates

## Problem

Branch roots can now maintain final-child inserts and eligible final-child
deletes, but updates still publish replacement index entries to the append-tail
overlay. Some updates can be maintained without general B-tree movement: the
source row already lives in the final child leaf, and the replacement `(key,
row_id)` still belongs in that same final child after replacing the old entry.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are needed.
- `mylite_storage_update_row_with_index_entry_changes_in_statement()` already
  plans dirty maintained index pages before writing the replacement row,
  row-state page, and fallback index-entry pages.
- Existing branch insert/delete helpers can rebuild one leaf from an entryset
  and refresh a branch child fence.
- `index_leaf_run_expected_entry_count()` still requires every non-final
  branch child leaf to be full, so updates that would move an entry across a
  child boundary are out of scope.

## Scope

- Plan a branch-root physical update only when:
  - the source row id is in the final child leaf;
  - the replacement index entry has the branch key size and index number;
  - replacing the source entry keeps the final leaf on one page; and
  - the rebuilt final leaf still sorts after the previous branch child fence.
- Rewrite the final leaf with the replacement entry, refresh the final branch
  child fence, and leave the branch page-owned entry count unchanged.
- Keep the normal row-state replacement page for logical visibility and
  recovery.
- Suppress fallback index-entry publication only for maintained branch updates.

## Non-Goals

- No updates that move entries into an interior child.
- No interior child update maintenance.
- No branch split, merge, redistribution, or file shrinking.
- No multi-level branch roots.

## Compatibility Impact

No SQL, public API, or storage-engine routing behavior changes. The change only
reduces append-tail index-entry growth for a supported branch-root physical
maintenance shape.

## Single-File And Lifecycle Impact

The branch root and final leaf are existing dirty pages protected by the
statement or transaction journal. The replacement row and row-state page remain
normal appended pages inside the primary `.mylite` file.

## Recovery Impact

Rollback and stale-journal recovery must restore the branch root fence and final
leaf contents when a maintained final-leaf update does not commit.

## Test Plan

- Extend branch-root storage coverage with a final child leaf that has at least
  two entries after a prior final-leaf delete.
- Cover statement rollback plus stale statement and transaction recovery for an
  eligible final-leaf update.
- Commit one eligible final-leaf update and verify branch entry count, child
  count, final leaf count, exact lookup, exact entrysets, full index reads, and
  row materialization.
- Run the storage and storage-smoke verification set.

## Acceptance Criteria

- Eligible final-leaf updates rewrite the branch root and final leaf while
  preserving the branch-root reader invariant.
- Fallback index-entry pages are suppressed only for maintained branch updates.
- Unsupported branch update shapes remain on the row-state/index-entry overlay
  path.
- Rollback and stale recovery restore dirty branch pages.

## Implementation Notes

- Update planning now recognizes single-level branch roots whose page-owned
  entry count maps to the current child count.
- A branch update plan protects the branch root and final child leaf before row
  append creates the statement or transaction journal.
- The writer rebuilds the final child leaf from its live leaf entries plus the
  replacement entry, refreshes the final branch fence, and leaves the branch
  entry count unchanged.
- Replacement entries are only maintained physically when their predicted row id
  and key still sort after the previous branch child fence. Other update shapes
  keep the existing append-tail replacement path.

## Verification Results

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`

## Risks And Follow-Ups

- This still leaves the replacement row-state page for logical history and
  cache integration, so it is not compaction.
- Updates that cross child boundaries remain merge/redistribution work.

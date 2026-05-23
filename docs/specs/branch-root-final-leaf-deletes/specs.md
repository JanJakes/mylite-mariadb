# Branch Root Final Leaf Deletes

## Problem

Branch-root inserts can now maintain the final child through in-place inserts,
splits, and tail refolds, but deletes still publish row-state overlay for
branch-rooted indexes. Some deletes can be maintained without general B-tree
merge logic: when the deleted row lives in the final child leaf and the branch
still needs the same number of children after the removal.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are needed.
- `index_leaf_run_expected_entry_count()` requires every non-final branch child
  leaf to be full. Deleting from an interior child would violate that invariant
  unless later leaves are redistributed.
- Deleting the only entry from the final child would reduce the expected branch
  child count and requires removing the final branch child cell, which belongs
  with merge/rebalance work.
- `mylite_storage_delete_row()` already plans dirty maintained root pages before
  writing the row-state delete page and can extend that dirty-page set to cover
  branch root plus final leaf pages.

## Scope

- Plan a branch-root physical delete only when:
  - the matching row id is in the final child leaf;
  - the final leaf has more than one entry; and
  - `branch.entry_count - 1` still maps to the same branch child count.
- Rewrite the final leaf without the deleted row id, refresh the final branch
  child fence, and decrement the branch page-owned entry count.
- Keep row-state delete publication in place for visibility, caches, and all
  unsupported branch delete shapes.

## Non-Goals

- No interior child delete maintenance.
- No branch child removal when the final leaf becomes empty.
- No merge, redistribution, or file shrinking.
- No update maintenance.

## Compatibility Impact

No SQL, public API, or storage-engine routing behavior changes. This changes
only the physical index maintenance path for supported branch-rooted indexes.

## Single-File And Lifecycle Impact

The branch root and final leaf are existing dirty pages protected by the
statement or transaction journal. The normal row-state delete page is still
appended and published after the physical maintenance so rollback and recovery
restore both page contents and logical visibility.

## Recovery Impact

Rollback and stale-journal recovery must restore the branch root entry count,
final child fence, and final leaf contents when a maintained final-leaf delete
does not commit.

## Test Plan

- Extend branch-root storage coverage with a final child leaf that has at least
  two entries.
- Delete one final-leaf row and verify branch entry count, child count, final
  leaf count, exact lookup, and full index reads.
- Cover statement rollback plus stale statement and transaction recovery for
  the maintained branch delete path.
- Run the storage and storage-smoke verification set.

## Acceptance Criteria

- Eligible final-leaf deletes rewrite the branch root and final leaf while
  preserving the branch-root reader invariant.
- Ineligible deletes remain on the row-state overlay path.
- Rollback and stale recovery restore dirty branch pages.
- Docs and roadmap keep this scoped separately from general split/merge work.

## Implementation Notes

- `plan_maintained_index_root_deletes()` now recognizes single-level branch
  roots and adds the branch root plus final child leaf to the protected dirty
  page set when the deleted row id is in that final leaf.
- The writer removes the leaf cell, rewrites the final branch child fence to
  the new leaf high key, and decrements the branch-owned entry count before the
  normal row-state delete page is appended.
- Unsupported shapes deliberately stay on the row-state overlay path: interior
  child deletes, deleting the only final-leaf entry, multi-level branches, and
  cases that would overflow the protected-page journal list.
- Existing branch refold coverage now uses an interior delete so the new
  final-leaf physical delete path does not hide the tail-overlay scenario.

## Verification Results

- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `git diff --check`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`

## Risks And Follow-Ups

- Physical delete maintenance still leaves the row-state page for logical
  history and cache integration, so it is not full compaction.
- Deleting the last entry from a final child remains merge work.

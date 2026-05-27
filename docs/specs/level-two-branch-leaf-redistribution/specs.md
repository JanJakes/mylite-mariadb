# Level Two Branch Leaf Redistribution

## Problem

Level-`2` branch roots can maintain fitting lower-leaf inserts and split full
lower leaves when the lower level-`1` branch has child capacity. They still
fall back to append-only index-entry pages when the selected lower leaf is full
but sibling leaves in the same lower branch have slack. Single-level branch
roots already handle this shape with bounded leaf-range redistribution,
including when a live append-tail overlay exists.

That fallback is now visible in the prepared-insert profile after large
whole-branch refolds were capped: append-only index-entry pages dominate the
remaining write volume.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB insert execution reaches MyLite through:
  - `mariadb/sql/sql_insert.cc:Write_record::single_insert()`
  - `mariadb/sql/handler.cc:handler::ha_write_row()`
  - `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()`
  - `mylite_storage_append_row_with_index_entries()` for durable routed tables.
- MyLite storage refs:
  - `plan_level_two_branch_index_root_insert()` currently returns without a
    maintained plan when the selected leaf is full and the selected lower
    branch has total slack.
  - `try_plan_branch_leaf_range_insert_redistribution()` and
    `redistribute_branch_index_leaf_range_entry()` already implement bounded
    single-level leaf-range redistribution.
  - `insert_level_two_branch_index_leaf_entry()` already refreshes the lower
    branch and then refreshes the root branch fence for level-`2` fitting and
    split inserts.

## Design

Add a level-`2` redistribution plan for this exact shape:

1. During level-`2` insert planning, when the selected lower leaf is full but
   the lower branch entry count is below its leaf capacity, scan a bounded
   contiguous leaf range inside that lower branch for slack.
2. If the range can absorb the inserted entry without adding a leaf page, plan
   a lower-branch redistribution instead of leaving the index entry on the
   append-only fallback path.
3. Protect the level-`2` root branch page, selected lower branch page, and the
   selected leaf range in the statement or transaction journal.
4. During writing, redistribute the selected lower branch leaf range in place,
   refresh the lower branch child fences and entry count, then refresh the root
   branch child fence and entry count.

The plan intentionally does not append new static pages, so an existing live
append-tail overlay remains after the static branch tree and stays visible to
the existing overlay readers.

## Non-Goals

- No lower-branch split beyond the existing no-overlay split path.
- No level-`2` root split or whole-root refold.
- No redistribution across lower branch boundaries.
- No update/delete maintenance changes.
- No file-format, public API, SQL, or storage-engine-routing changes.

## Compatibility Impact

SQL-visible results and handler ordering remain unchanged. The storage layer
only changes an internal publication path for supported fixed-width index
inserts that already have a maintained level-`2` branch root.

## Single-File And Embedded Lifecycle

All durable state remains in the primary `.mylite` file. Existing dirty branch
and leaf pages are journal-protected before mutation, and the inserted row page
is appended through the existing row write path. Because redistribution reuses
existing leaf pages, it does not move the branch tail past live overlay pages.

## Binary-Size, License, And Dependency Impact

No dependency or license changes. Binary-size impact is limited to a bounded
planner path, a level-`2` writer variant, and focused tests.

## Test And Verification Plan

- Add storage coverage that builds a level-`2` root whose selected lower leaf is
  full while a sibling leaf in the same lower branch has slack.
- Include a live append-tail index-entry overlay before the insert to prove the
  redistribution does not hide existing overlay pages.
- Commit a rightmost lower-branch insert that raises the lower branch maximum
  key to prove a buffered statement refreshes the parent root fence from the
  rewritten lower branch page.
- Assert that the insert grows the file by one row page, not by a fallback
  index-entry page, and that the lower branch and root entry counts update.
- Verify exact lookup and full index-entry ordering after the redistribution.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Eligible full-leaf level-`2` inserts redistribute a bounded lower-branch leaf
  range instead of appending a fallback index-entry page.
- Existing append-tail overlays remain visible.
- Root and lower-branch fences and entry counts remain valid.
- Rollback restores the old branch pages and file size.
- Existing branch maintenance and storage-smoke tests keep passing.

## Verification

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `./build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `143.70 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `179.40 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed with prepared insert step at `25.048 us/op`, commit at `202.532 ms`,
  and final page count `72843`.

A kept inspection run for the same phase had `65329` append-only index-entry
pages, `7089` row pages, `409` index leaf pages, `6` index branch pages,
`7` catalog pages, `2` table-definition blob pages, and `1` header page. This
confirms the slice is correct but only marginal for the current 100k
prepared-insert bottleneck; append-tail index-entry volume remains the next
measured target.

## Risks

- The slice only handles lower branches with existing slack. Packed lower
  branches with live overlays still need broader overlay cleanup or a
  multi-level snapshot publication path.
- The planner rereads a bounded sibling range during planning; active leaf
  caches should keep repeated same-statement planning from redoing that work.

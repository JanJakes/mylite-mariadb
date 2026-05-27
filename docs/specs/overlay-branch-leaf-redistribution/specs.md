# Overlay Branch Leaf Redistribution

## Problem

Prepared insert profiling still shows `refold_branch_index_root_insert()` as
the dominant branch-index maintenance cost. One reason is conservative planning:
`try_plan_branch_leaf_range_insert_redistribution()` refuses to redistribute a
full static leaf range when the branch has a live append-tail overlay. That
forces a full branch refold even when the static leaf range has enough physical
slack for the new entry.

The overlay does not have to be folded into the static branch for every insert.
If the static branch leaves can accept the current entry by redistribution, the
new entry can become part of the static branch while the existing live or dead
tail overlay remains visible through the overlay-aware read path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB row writes still arrive through handler calls
  (`mariadb/sql/handler.cc:handler::ha_write_row()`), and
  `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()` delegates
  durable routed writes to MyLite storage.
- This slice changes first-party MyLite storage only:
  `packages/mylite-storage/src/storage.c` and storage tests.
- `plan_branch_index_root_insert()` already supports direct insertion into
  non-full branch leaves while append-tail overlays exist.
- `try_plan_branch_leaf_range_insert_redistribution()` currently probes
  `index_branch_tail_has_live_overlay()` and returns without planning whenever
  the branch has overlay rows or row-state pages after the static leaf run.
- `redistribute_branch_index_leaf_range_entry()` rewrites only selected static
  leaf pages and refreshes branch fences; it does not consume append-tail
  overlay pages.
- Overlay-aware index reads already merge or filter tail pages after branch
  reads, so keeping the overlay after a static redistribution preserves the
  same visibility model used by non-full branch-leaf inserts.

## Design

Allow branch leaf-range redistribution to plan against the static leaf range
without rejecting branches that have a live append-tail overlay:

- remove the overlay gate from the redistribution planner;
- keep the existing range fit check based on static leaf entry counts and leaf
  capacity;
- keep the current write path, which rewrites only static branch leaves and
  branch fences;
- leave split and promotion gates conservative when overlays exist; and
- add a regression test that appends a same-index overlay page, then inserts
  into a full static leaf whose sibling range has slack, proving the static
  redistribution happens while the overlay entry remains readable.

## Non-Goals

- No branch split or multi-level promotion with live overlays.
- No consumption or compaction of existing append-tail overlay entries.
- No change to row-state overlay semantics or duplicate filtering.
- No file-format change.

## Compatibility Impact

No SQL, C API, handler, wire-protocol, or metadata behavior changes. The change
only chooses a cheaper existing physical maintenance path for a case that
previously refolded the same logical index contents.

## Single-File And Lifecycle Impact

Durable state remains in the primary `.mylite` file. Static leaf rewrites stay
under the existing preplanned dirty-page journal protection, and append-tail
overlay pages remain part of the active checkpoint visibility model.

## Build, Size, And Dependencies

Small first-party C and test change only. No new dependency or embedded profile
change.

## Test Plan

- Add storage coverage for branch leaf redistribution with a same-index tail
  overlay that remains visible after the static branch leaf rewrite.
- Keep existing rollback and committed redistribution tests passing.
- Run storage unit coverage.
- Run storage-smoke embedded storage-engine coverage.
- Run the prepared insert component benchmark as local performance evidence.
- Run `git diff --check` and `git clang-format --diff` on touched C files.

## Acceptance Criteria

- Branch leaf-range redistribution can plan when the branch has a live overlay.
- The rewritten static branch root remains valid and keeps expected child
  fences.
- Existing overlay entries remain visible through index entry reads.
- Existing storage and routed storage-engine tests pass.
- Prepared insert profiling shows fewer full refold samples or no local
  regression.

## Risks And Open Questions

- This handles redistribution only; split and promotion paths with overlays
  remain conservative.
- Branch page `entry_count` can represent physical static entries while catalog
  root counts represent logical visible entries. Existing overlay-aware readers
  already tolerate count differences, but this slice keeps tests focused on the
  covered single-level redistribution case.

## Verification Results

- `git diff --check` passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  reported no formatting changes.
- `cmake --build --preset dev --target mylite_storage_test` passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  passed in 140.27 seconds.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  passed.
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
  passed in 34.20 seconds.
- Four full local `prepared-insert-components` runs at 1,000 setup rows and
  10,000 measured iterations reported prepared insert step costs of 21.860,
  23.965, 22.772, and 21.977 us/op, with final database size 3,657,728 bytes
  and 893 pages. Four follow-up step-only samples reported 19.601, 19.637,
  19.828, and 20.960 us/op.
- A 100,000-iteration profiler sample still showed
  `refold_branch_index_root_insert()` as the broad prepared-insert hotspot, so
  this slice removes one overlay gate but does not complete the remaining
  branch-refold performance work.

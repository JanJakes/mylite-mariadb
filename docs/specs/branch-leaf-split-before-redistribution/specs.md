# Branch Leaf Split Before Broad Redistribution

## Problem

Prepared insert execution is still dominated by single-level branch maintenance
after eligible full leaves can split before whole-root refold. A local sampled
run of:

```sh
build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components --profile-iterations=200000 1000
```

measured `220.407 us/op` for the prepared insert step. The sample still spent
most storage time below `mylite_storage_append_row_with_index_entries()` in:

- `plan_branch_index_root_insert()` ->
  `try_plan_branch_leaf_range_insert_redistribution()`, especially leaf reads,
  checksums, and bounded range scans;
- `write_branch_index_root_inserts()` ->
  `redistribute_branch_index_leaf_range_entry()`, especially leaf decode,
  resort, checksum, and page writes; and
- the remaining `branch_index_refold_insert_fits()` /
  `refold_branch_index_root_insert()` fallback checks.

The current policy preserves branch child count first: when a selected
single-level branch leaf is full but the branch has total entry slack, the
planner scans for bounded redistribution before it considers splitting the
selected leaf. Adjacent redistribution is cheap and useful, but broad range
redistribution is not always the cheapest steady-state insert mutation when the
branch still has child-cell capacity.

## Goal

Prefer a selected-leaf split before broad range redistribution when a level-`1`
branch:

- targets a full selected leaf,
- has no adjacent sibling redistribution plan,
- has child-cell capacity for one more leaf,
- has no live append-tail overlay that would be hidden by appending that leaf,
  and
- can fit the split inside the existing rollback-journal protected-page budget.

Adjacent redistribution remains the first choice when it can absorb the insert
with one sibling read. Broad redistribution remains available when a split is
unsafe or impossible. The result should move duplicate-heavy prepared insert
loops away from repeated broad redistribution scans and rewrites without
discarding the cheapest sibling redistribution case.

## Non-Goals

- No durable page-format change.
- No change to multi-level branch insert policy.
- No general B-tree balancing or merge policy.
- No removal of adjacent or bounded range redistribution.
- No change to live-overlay safety rules.
- No public `libmylite`, handler, SQL, or storage-engine routing behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- MariaDB insert execution reaches MyLite storage through:
  - `mariadb/sql/sql_insert.cc:Write_record::single_insert()`
  - `mariadb/sql/handler.cc:handler::ha_write_row()`
  - `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()`
  - `mariadb/storage/mylite/ha_mylite.cc` calls
    `mylite_storage_append_row_with_index_entries()` for durable routed tables.
- MyLite storage currently plans level-`1` branch inserts in
  `packages/mylite-storage/src/storage.c:plan_branch_index_root_insert()`.
- `try_plan_branch_leaf_range_insert_redistribution()` reads candidate leaves
  outward from the selected child to find a bounded range with slack.
- `redistribute_branch_index_leaf_range_entry()` rereads and decodes the branch
  root plus every leaf in the planned range, rebuilds a temporary entryset, and
  rewrites every leaf in that range.
- `split_branch_index_leaf_entry()` already performs the cheaper local mutation
  for a full selected leaf by rewriting the selected leaf and branch root and
  appending one new leaf.
- `index_branch_tail_has_live_overlay()` must still block the split because a
  new appended child leaf would otherwise move the branch subtree high page id
  past live row-state or index-entry tail pages.
- An initial local experiment that preferred splits before every redistribution
  case worsened the quick prepared-insert sample, so this slice preserves
  adjacent redistribution and only moves the split ahead of broader range
  scans.

## Compatibility Impact

No SQL compatibility claim changes. Rows inserted through routed `ENGINE=InnoDB`
and explicit MyLite tables keep the same ordering, duplicate-key semantics,
rollback behavior, handler write path, and requested/effective engine metadata.
This is an internal storage mutation-order change. `docs/COMPATIBILITY.md` does
not need a new claim.

## Design

In `plan_branch_index_root_insert()` for level-`1` branch roots:

1. Decode the selected leaf as today.
2. If the branch has total entry slack and a split could be possible, try only
   the adjacent right/left redistribution shape first.
3. When no adjacent redistribution plan is available, branch entry count is
   known, the branch has child-cell capacity, and the branch tail has no live
   overlay, plan the existing split-leaf mutation.
4. Try broader bounded range redistribution only when that split cannot be
   planned, for example because branch child cells are full.
5. Preserve the existing whole-root refold fallback after redistribution misses.

This intentionally keeps the cheapest child-count-preserving path but chooses a
local split over scanning and rewriting a wider range when both are safe. The
policy matches ordinary B-tree insert behavior more closely: split the full
target child when the parent has room, and use broad redistribution as a
space-saving fallback rather than as the hot path.

## File Lifecycle

Durable state remains in the primary `.mylite` file. The preferred split appends
one leaf page and rewrites the selected leaf plus branch root under the existing
rollback-journal lifecycle. No new companion files are introduced. Adjacent and
bounded redistribution plus refold keep their existing journal behavior when
they are still selected.

## Embedded Lifecycle And API

No public embedded lifecycle or `libmylite` API behavior changes.

## Build, Size, And Dependencies

No dependency, license, generated artifact, or embedded build-profile change.
Binary-size impact should be limited to a small planner branch and adjusted
storage tests.

## Test Plan

- Preserve adjacent redistribution coverage when a full selected child has
  immediate sibling slack, even if a split could also be planned.
- Update storage coverage so a full selected child with branch child capacity
  and only non-adjacent slack splits after the adjacent check and before broad
  range redistribution.
- Preserve explicit coverage where broad bounded redistribution still runs when
  splitting is not available.
- Preserve live-overlay fallback coverage.
- Preserve statement rollback coverage for the split path.
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

- Eligible level-`1` full selected leaves split before broad bounded range
  redistribution after adjacent redistribution misses.
- Adjacent redistribution remains preferred when it can absorb the insert.
- Broad bounded range redistribution remains reachable for split-blocked cases.
- Existing branch insert, redistribution, refold, rollback, and recovery tests
  pass after expectation updates.
- The prepared insert component benchmark records the new result or identifies
  the next measured bottleneck.

## Verification

- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed; 174.19 sec.
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`: passed.
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`: passed; 34.45 sec.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`: passed; prepared insert step component measured `101.526 us/op` locally.
- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`: passed.

## Risks And Open Questions

- Earlier splits can increase branch child count and tree height sooner than
  redistribution-first planning. Preserving adjacent redistribution reduces
  that risk, but future compaction/merge work still needs to address long-term
  density.
- The split path still decodes/checksums the selected leaf and branch root; this
  slice removes broad scan/redistribution work, not all page-validation cost.
- If a live overlay blocks appending a child, redistribution/refold must remain
  conservative so the overlay is not hidden behind a moved branch tail.

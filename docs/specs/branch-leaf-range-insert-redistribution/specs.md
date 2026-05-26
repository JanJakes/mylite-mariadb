# Branch Leaf Range Insert Redistribution

## Goal

Reduce duplicate-heavy prepared insert step cost for maintained single-level
branch roots when the selected child leaf is full, the adjacent sibling is also
full, but a nearby leaf in the same branch still has spare capacity.

The existing sibling redistribution handles the local two-leaf shape. The next
bounded step is to redistribute across a small contiguous run of existing leaf
pages instead of refolding the entire branch root through
`refold_branch_index_root_insert()`.

## Non-Goals

- No row-page packing or row-id allocation redesign.
- No new durable page format, catalog record, public C API, or storage-engine
  routing behavior.
- No multi-level branch redistribution. This slice covers level-`1` branch
  roots only.
- No delete/merge redistribution and no general recursive B-tree rebalancer.
- No redistribution when an append-tail row-state or index-entry overlay for
  the same table/index would be hidden.
- No unbounded range rewrite. The range must fit in the existing rollback
  journal page budget.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- MariaDB refs:
  - `mariadb/sql/sql_insert.cc:mysql_insert()` prepares and executes INSERT
    statements.
  - `mariadb/sql/sql_insert.cc:Write_record::single_insert()` dispatches
    accepted rows to `table->file->ha_write_row()`.
  - `mariadb/sql/handler.cc:handler::ha_write_row()` calls the storage
    handler's `write_row()` after handler-level checks.
  - `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()` prepares
    MyLite row and index entries and calls
    `mylite_storage_append_row_with_index_entries()`.
- MyLite refs:
  - `packages/mylite-storage/src/storage.c:mylite_storage_append_row_with_index_entries()`
    plans maintained index updates before row/index publication.
  - `packages/mylite-storage/src/storage.c:plan_branch_index_root_insert()`
    routes full selected leaves with total branch slack through bounded
    leaf-range redistribution first, then the whole-root refold fallback.
  - `packages/mylite-storage/src/storage.c:try_plan_branch_leaf_range_insert_redistribution()`
    handles bounded adjacent and non-adjacent leaf-range redistribution.
  - `packages/mylite-storage/src/storage.c:redistribute_branch_index_leaf_range_entry()`
    rewrites existing leaves in the planned range, refreshes branch fences, and
    stages the branch root in the maintained-insert dirty page buffer.
  - `packages/mylite-storage/src/storage.c:refold_branch_index_root_insert()`
    reads every branch leaf through `read_branch_index_root_entries()`, appends
    the new entry, and writes a fresh branch snapshot.

Local sampling of
`build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`
after sibling redistribution still showed most remaining prepared insert step
time under `refold_branch_index_root_insert()` and
`read_branch_index_root_entries()`. The sampled path spent time decoding many
branch leaves and rebuilding live row-id sets. That is evidence that some
duplicate-key insert shapes still have branch-level slack, but not in an
immediate sibling leaf.

## Compatibility Impact

No SQL, C API, metadata, or engine-routing compatibility claim changes. MariaDB
still calls the MyLite handler through the same insert path, and routed
`ENGINE=InnoDB` tables still resolve to MyLite storage under the existing
routing policy. `docs/COMPATIBILITY.md` does not need a new claim.

## Design

Add a maintained insert plan shape for single-level branch roots where:

- the selected child leaf is full,
- the branch root is level `1`,
- the branch has total entry slack
  (`entry_count + 1 <= child_count * leaf_capacity`),
- a contiguous range from the selected leaf to a nearby leaf with spare
  capacity fits within the protected-page budget,
- the selected range plus the new entry fits in the same number of leaf pages,
  and
- `index_branch_tail_has_live_overlay()` reports no live tail overlay for the
  table/index.

Search right from the selected child first, then left. The first eligible range
is planned. Planning records the range start child index and range length, and
protects the branch root plus every leaf page in the range. If no eligible
bounded range is found, the existing whole-root refold path remains the
fallback.

Execution should:

1. Read and decode the branch root and every leaf in the planned child range.
2. Append all range leaf entries into a temporary entryset, insert the new
   `(key, row_id)` in sorted order, and split the sorted entries back across
   the same leaf page ids.
3. Preserve branch child order by assigning the first sorted segment to the
   first child in the planned range and continuing left to right.
4. Refresh every changed branch child fence and increment the branch entry
   count by one.
5. Write leaf pages through the existing immediate path and write the branch
   root through the maintained insert routing-page buffer.

The range distribution should keep each rewritten leaf non-empty and should
spread entries across the range rather than packing all slack into the final
leaf. This preserves lookup ordering and leaves local slack for subsequent
duplicate-key inserts.

## File Lifecycle

Durable state remains in the primary `.mylite` file. No new companion files are
introduced. Existing statement, savepoint, or transaction journals protect the
branch root plus every rewritten leaf before mutation. Active checkpoint dirty
page buffers may stage the branch root, while leaf pages keep the immediate
write path.

## Embedded Lifecycle And API

No public `libmylite` lifecycle or API behavior changes. The slice only changes
the storage implementation beneath existing handler `write_row()` calls.

## Build, Size, And Dependencies

No dependency, license, generated artifact, or embedded build-profile change.
The expected binary-size impact is limited to first-party storage helpers and
tests.

## Test Plan

- Add storage coverage for a single-level branch root with three leaves where
  an insert targets a full selected leaf, the adjacent sibling is full, and a
  farther sibling in the same direction has spare capacity.
- Verify:
  - no fallback index-entry page is written for the handled index,
  - the branch child count is unchanged,
  - the branch entry count increases by one,
  - every rewritten child fence is refreshed,
  - logical active reads and committed reads see sorted index entries, and
  - close/reopen preserves indexed lookup order.
- Cover statement rollback for the range path.
- Keep adjacent right/left sibling redistribution coverage passing.
- Keep the live-overlay fallback behavior unchanged.
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

- Eligible full-child single-level branch inserts with bounded non-adjacent
  range slack no longer call `refold_branch_index_root_insert()`.
- Logical active reads and committed reads see the redistributed index entries.
- Rollback restores every rewritten range leaf plus the branch root.
- Existing adjacent-sibling behavior remains covered.
- Existing live-overlay cases still avoid range redistribution and keep the
  refold or fallback behavior.
- The performance baseline records a reduced prepared insert step sample or
  documents the next measured bottleneck.

## Verification Results

- `cmake --build --preset dev --target mylite_storage_test` passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure` passed
  in 158.68 seconds after formatting.
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`
  passed.
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
  passed in 32.90 seconds.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`
  passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`
  passed with local prepared insert step timing at 686.129 us/op and final
  commit at 27.330 ms.
- `git diff --check` passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  passed.

## Risks And Open Questions

- This remains a bounded local redistribution, not production recursive B-tree
  split/merge/navigation.
- A wider range rewrites more leaf pages per insert. The range must stay within
  the rollback-journal page budget and should prefer the nearest slack leaf.
- Fence refresh must preserve strict branch ordering for duplicate-key groups
  split across multiple leaves.
- A bad overlay guard could hide live append-tail row-state or index-entry
  changes. Keep redistribution disabled when same-table/index overlay exists.

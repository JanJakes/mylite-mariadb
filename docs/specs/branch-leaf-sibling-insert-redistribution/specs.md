# Branch Leaf Sibling Insert Redistribution

## Goal

Reduce duplicate-heavy prepared insert step cost for maintained single-level
branch roots. When the selected child leaf is full but an adjacent sibling leaf
has capacity, MyLite should redistribute the selected leaf, sibling leaf, and
new entry across those two existing leaf pages instead of refolding the entire
branch root into a fresh leaf snapshot.

This targets the measured `prepared-insert-components` hotspot where
`refold_branch_index_root_insert()` reads and decodes every branch leaf for many
inserts into low-cardinality secondary-key groups.

## Non-Goals

- No row-page packing or row-id allocation redesign.
- No new durable page format, catalog record, public C API, or engine-routing
  behavior.
- No multi-level branch redistribution. This slice covers single-level branch
  roots only.
- No delete/merge redistribution and no general B-tree rebalancer.
- No redistribution when an append-tail row-state or index-entry overlay for
  the same table/index would be hidden.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- MariaDB refs:
  - `mariadb/sql/sql_insert.cc:mysql_insert()` prepares and executes INSERT
    statements.
  - `mariadb/sql/handler.cc:handler::ha_write_row()` calls the storage
    handler's `write_row()` after in-server constraint checks.
  - `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()` prepares
    MyLite index entries and calls
    `mylite_storage_append_row_with_index_entries()`.
- MyLite refs:
  - `packages/mylite-storage/src/storage.c:mylite_storage_append_row_with_index_entries()`
    plans maintained index updates before row/index publication.
  - `packages/mylite-storage/src/storage.c:plan_branch_index_root_insert()`
    routes full selected leaves with total branch slack to the refold path.
  - `packages/mylite-storage/src/storage.c:refold_branch_index_root_insert()`
    reads all branch entries through `read_branch_index_root_entries()`, appends
    the new entry, and rewrites a fresh branch snapshot.
  - `packages/mylite-storage/src/storage.c:insert_branch_index_leaf_entry()`
    already handles the simpler case where the selected leaf has spare
    capacity.
  - `packages/mylite-storage/src/storage.c:split_branch_index_leaf_entry()`
    already handles the case where all leaves are full and the branch can add a
    new child.

Local sampling of
`build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`
showed the prepared insert step mostly under `refold_branch_index_root_insert()`
and `read_branch_index_root_entries()`, with time spent decoding branch leaves
and rebuilding live row-id sets. The workload uses a low-cardinality secondary
index, so duplicate-key inserts repeatedly fill one child before the whole
branch root is globally full.

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
- the selected leaf has an immediate left or right sibling in the branch,
- the selected leaf plus sibling leaf plus new entry fit in two leaf pages, and
- `index_branch_tail_has_live_overlay()` reports no live tail overlay for the
  table/index.

Prefer an eligible right sibling when present, otherwise use an eligible left
sibling. Planning records both existing leaf page ids and protects the sibling
page in the statement journal.

Execution should:

1. Read and decode the branch root, selected leaf, and sibling leaf.
2. Append both leaves into a small entryset, insert the new `(key, row_id)` in
   sorted order, and split the sorted entries back across the same two page ids.
3. Preserve branch child order: when redistributing with the right sibling,
   write the selected page as the left page and the sibling as the right page;
   with the left sibling, write the sibling as the left page and the selected
   page as the right page.
4. Refresh both branch child fences and increment the branch entry count by
   one.
5. Write leaf pages through the existing immediate path and write the branch
   root through the maintained insert routing-page buffer.

The existing whole-root refold path remains the fallback when no eligible
sibling exists or a live append-tail overlay must be preserved.

## File Lifecycle

Durable state remains in the primary `.mylite` file. No new companion files are
introduced. Existing rollback or transaction journals protect the selected
leaf, sibling leaf, and branch root before mutation. Active checkpoint dirty
page buffers may stage the branch root, while leaf pages keep the immediate
write path.

## Embedded Lifecycle And API

No public `libmylite` lifecycle or API behavior changes. The slice only changes
the storage implementation beneath existing handler `write_row()` calls.

## Build, Size, And Dependencies

No dependency, license, generated artifact, or embedded build-profile change.
The expected binary-size impact is limited to a small first-party storage helper
and tests.

## Test Plan

- Add storage coverage for a single-level branch root where an insert targets a
  full child leaf while the adjacent sibling has spare capacity, verifying:
  - the insert remains visible before commit,
  - no fallback index-entry page is written for that index,
  - the branch child count is unchanged,
  - the branch entry count increases by one,
  - both affected child fences are refreshed, and
  - reopen preserves indexed lookup order.
- Cover rollback for the redistribution path, including active transaction and
  nested savepoint rollback where practical.
- Verify the existing refold fallback still covers live-overlay cases.
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

- Eligible full-child single-level branch inserts with adjacent sibling slack no
  longer call `refold_branch_index_root_insert()`.
- Logical active reads and committed reads see the redistributed index entries.
- Rollback restores selected and sibling leaf pages plus the branch root.
- Existing live-overlay cases still avoid local redistribution and keep the
  refold or fallback behavior.
- The performance baseline records a reduced prepared insert step sample or
  documents the next measured bottleneck.

## Verification Results

Local environment: macOS worktree, `dev` and `storage-smoke-dev` presets.

- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `148.75 sec`.
- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c
  packages/mylite-storage/tests/storage_test.c`: passed.
- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_storage_engine_test`: passed.
- `ctest --test-dir build/storage-smoke-dev -R
  libmylite.embedded-storage-engine --output-on-failure`: passed in
  `31.20 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`:
  passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-insert-components 1000 10000` reported the prepared insert
  step component at `1311.769 us/op` and commit at `935.009 ms`.
- A repeated sampled run reported `1289.607 us/op` for the prepared insert step
  and `308.160 ms` for commit. The sample still showed most remaining step
  time under `refold_branch_index_root_insert()` /
  `read_branch_index_root_entries()`, so broader non-adjacent redistribution,
  live-overlay handling, or a real mutable B-tree remains next.

## Risks And Open Questions

- Fence refresh must preserve strict branch ordering for duplicate-key groups
  split across adjacent leaves.
- A bad overlay guard could hide live append-tail row-state or index-entry
  changes. Keep redistribution disabled when same-table/index overlay exists.
- This does not address deeper branch roots, row-page packing, or commit-time
  append-buffer flushing, so it is only one insert-performance step.

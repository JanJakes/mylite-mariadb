# Branch Leaf Range Plan Scan

## Goal

Reduce prepared insert step cost after branch leaf range redistribution by
avoiding repeated decode/checksum work while searching for a bounded slack leaf
range.

The previous slice generalized adjacent sibling redistribution to a bounded
contiguous leaf range. Local sampling of
`build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components --profile-iterations=200000 1000`
then showed most remaining storage-side planning time under
`try_plan_branch_leaf_range_insert_redistribution_candidate()`, with repeated
`decode_index_leaf_page()` and `checksum_page()` work while each larger
candidate range reread leaves already inspected by the previous candidate.

## Non-Goals

- No durable page-format change and no branch-cell child entry-count field.
- No new public C API, handler API, SQL behavior, metadata claim, or storage
  routing behavior.
- No multi-level branch redistribution.
- No change to live append-tail overlay fallback rules.
- No new unbounded B-tree rebalancer.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- MariaDB insert path remains unchanged:
  - `mariadb/sql/sql_insert.cc:mysql_insert()`
  - `mariadb/sql/sql_insert.cc:Write_record::single_insert()`
  - `mariadb/sql/handler.cc:handler::ha_write_row()`
  - `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()`
- MyLite refs:
  - `packages/mylite-storage/src/storage.c:plan_branch_index_root_insert()`
    calls the leaf-range redistribution planner before the whole-root refold
    fallback.
  - `packages/mylite-storage/src/storage.c:try_plan_branch_leaf_range_insert_redistribution()`
    currently probes right ranges, then left ranges.
  - `packages/mylite-storage/src/storage.c:try_plan_branch_leaf_range_insert_redistribution_candidate()`
    rereads and decodes every leaf in each candidate range.
  - `packages/mylite-storage/src/storage.c:append_maintained_index_insert_plan_branch_range_redistribution()`
    records only the selected range start and range length; execution rereads
    pages from the branch root, so planning can avoid retaining page bytes.

## Compatibility Impact

No compatibility claim changes. SQL still enters through MariaDB prepared
statement execution and the MyLite handler write path, and routed
`ENGINE=InnoDB` tables still resolve to MyLite under the existing routing
policy.

## Design

Replace repeated candidate probing with a bounded one-pass scan per direction.

For a full selected leaf in a level-`1` branch root:

1. Run the existing live-overlay guard once.
2. Scan right from the selected child, reading each leaf at most once into a
   small local planning array containing child index, page id, and entry count.
3. Stop when the range reaches the journal protected-page limit, the branch
   ends, an invalid leaf is found, or a non-selected leaf with spare capacity
   gives an eligible range.
4. If no right range is eligible, scan left with the same rules.
5. Append the first eligible plan using the existing range start and leaf page
   ids. Execution remains responsible for rereading protected pages and
   rebuilding the leaf contents.

The planner must preserve the prior search preference: nearest eligible right
range first, then nearest eligible left range. It must still require:

- the selected leaf is full,
- at least one non-selected scanned leaf has spare capacity,
- all scanned leaves match table id, index number, and key size,
- range entry count plus the new entry fits in the same number of leaves, and
- the range plus root fits within the protected-page journal budget.

## File Lifecycle

Durable state remains in the primary `.mylite` file. No new companion files are
introduced. The scan only changes planning-time reads and local stack state.

## Embedded Lifecycle And API

No public `libmylite` lifecycle or API behavior changes. The slice only changes
storage planning below existing handler `write_row()` calls.

## Build, Size, And Dependencies

No dependency, license, generated artifact, or embedded build-profile change.
The binary-size impact should be limited to small first-party storage planner
helpers and test assertions.

## Test Plan

- Keep branch leaf sibling and non-adjacent range redistribution tests passing.
- Add or strengthen storage coverage so a non-adjacent right range still
  redistributes through the first eligible range after the planner rewrite.
- Add left-side coverage if the scan helper structure makes that path easier to
  regress than the existing adjacent left-sibling coverage.
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

- Planning a right or left branch leaf range decodes each scanned candidate
  leaf at most once.
- Eligible non-adjacent ranges still avoid whole-root refolds.
- Existing adjacent sibling redistribution behavior remains covered.
- Live-overlay cases still avoid redistribution and preserve fallback behavior.
- Local prepared insert component timing records the new result or identifies
  the next measured bottleneck.

## Risks And Open Questions

- The scan still performs leaf checksums for every scanned leaf. Avoiding that
  without a durable per-child count field is a separate file-format decision.
- The planner must not cache page bytes for execution because rollback
  protection and active dirty buffers own the execution-time page view.
- Right-first search order is a product choice from the previous slice; this
  slice should not change it while optimizing the implementation.

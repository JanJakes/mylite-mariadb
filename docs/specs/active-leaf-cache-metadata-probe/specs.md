# Active Leaf Cache Metadata Probe

## Problem

After packed index-entry cache sets reduced prepared-insert file growth, a
fresh local sample of:

```sh
build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components --profile-iterations=10000000 1000
```

still shows maintained-index planning under
`try_plan_branch_leaf_range_insert_redistribution()` as a material insert-step
cost. The hot leaf-range planner calls `read_branch_leaf_range_plan_scan_leaf()`
for sibling candidates. When those leaves are already in the active index leaf
page cache, the cache hit still copies the full `4096`-byte page into a stack
buffer even though the range planner only needs table id, index number, key
width, and entry count.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through:
  - `mariadb/sql/sql_insert.cc:Write_record::single_insert()`;
  - `mariadb/sql/handler.cc:handler::ha_write_row()`;
  - `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()`.
- `packages/mylite-storage/src/storage.c::read_branch_leaf_range_plan_scan_leaf()`
  currently calls `read_cached_active_index_leaf_page()`.
- `read_cached_active_index_leaf_page()` must copy the full page because most
  callers need payload bytes, but branch range planning only needs cached
  metadata for candidate leaves.
- The active leaf cache entry already stores the required metadata next to the
  cached page bytes.

## Design

Add a metadata-only active leaf-cache probe:

- keep `read_cached_active_index_leaf_page()` unchanged for callers that need
  page bytes and payload pointers;
- add a small cached leaf summary type containing table id, index number, key
  width, and entry count;
- add `read_cached_active_index_leaf_page_summary()` that validates the page id,
  probes the active leaf cache, and fills the summary without copying the page;
- make `read_branch_leaf_range_plan_scan_leaf()` use the summary path first;
- retain the existing file read, decode, counter, and cache-store path on cache
  misses.

## Non-Goals

- No branch redistribution policy change.
- No cache size, eviction, durable cache, or page format change.
- No public API, handler API, SQL behavior, or storage-engine routing change.
- No attempt to remove the later writer's page copy when it truly needs bytes.

## Compatibility Impact

No SQL-visible compatibility change. The planner still validates the same table
id, MariaDB key number, key width, and capacity bounds before accepting a
candidate leaf count.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, recovery, lock, or companion-file behavior changes.
The change reads transient active-statement cache metadata that already belongs
to the statement lifecycle and is cleared by existing active cache invalidation.

## Public API, File-Format, Binary-Size, License, And Dependency Impact

No public API, file-format, dependency, or license change. Binary-size impact
is limited to one small helper and summary struct.

## Test And Verification Plan

- Keep active branch leaf plan-cache coverage passing; it asserts that the
  second same-statement range plan uses the active cache instead of reading
  another leaf page from disk.
- Add a focused test hook that drives branch leaf-range planning from an active
  cached leaf and asserts that the planner uses a metadata summary cache hit
  without reading the page from disk.
- Run:
  - `git diff --check`;
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`;
  - `cmake --build --preset dev --target mylite_storage_test`;
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`;
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`;
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`;
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`.

## Acceptance Criteria

- Branch leaf-range planning avoids full cached-page copies on active leaf-cache
  hits.
- Cache misses keep the existing read, decode, validation, and cache-store
  behavior.
- Existing branch planning, redistribution, rollback, and routed storage tests
  pass.
- Prepared insert component timing and sample evidence are recorded.

## Verification Results

Initial local sample evidence before the slice showed
`read_branch_leaf_range_plan_scan_leaf()` reaching
`read_cached_active_index_leaf_page()`, with the active cache hit copying full
leaf pages in `_platform_memmove`. After the slice, a matching short sample
showed the same branch range planner reaching
`read_cached_active_index_leaf_page_summary()` instead, with no full-page copy
under the range-plan summary path. Remaining full-page leaf-cache copies are in
callers that still need page bytes.

Local timing remained noisy at 100k iterations, but the longer
`prepared-insert-components --profile-iterations=1000000 1000` run improved
from the prior `10.134 us/op` prepared insert step sample to `8.757 us/op`.
The file shape remained stable at `299,372,544` final bytes and `73,089`
header pages for that 1M-row sample.

The final committed-tree 100k prepared-insert component run reported
`24.454 us/op` for the prepared insert step, `19.099 ms` for the prepared
insert commit, `31,653,888` final bytes, and `7,728` header pages.

## Risks And Unresolved Questions

- The helper must not expose stale metadata after active cache invalidation.
  Reusing the existing active cache lookup keeps the same owner and invalidation
  boundary.
- This is a CPU/memory-traffic reduction only. It does not remove the remaining
  branch redistribution decisions, writer page copies, or checksum refreshes.

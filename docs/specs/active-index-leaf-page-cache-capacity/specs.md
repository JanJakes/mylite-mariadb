# Active Index Leaf Page Cache Capacity

## Problem

After preserving branch-refold caches across insert retargets, the 100k
prepared-insert component sample still reports:

- branch leaf-range plan reads: `1458`
- level-two branch leaf plan reads: `218`

Local diagnostics around `read_branch_leaf_range_plan_scan_leaf()` showed
`1458` range-plan page reads but only `1113` unique leaf page ids. The current
active index leaf-page cache limit is `1024` full pages, so the prepared insert
workload churns useful planning pages out of the cache and rereads them later.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB insert execution still reaches MyLite through:
  - `mariadb/sql/sql_insert.cc:Write_record::single_insert()`
  - `mariadb/sql/handler.cc:handler::ha_write_row()`
  - `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()`
- MyLite refs:
  - `packages/mylite-storage/src/storage.c:read_branch_leaf_range_plan_scan_leaf()`
    checks the active leaf-page cache before reading and decoding a sibling
    leaf page for redistribution planning.
  - `packages/mylite-storage/src/storage.c:read_branch_insert_plan_leaf_page()`
    uses the same cache for selected leaf pages under branch insert planning.
  - `packages/mylite-storage/src/storage.c:store_active_index_leaf_page_for_file()`
    recycles an entry once `MYLITE_STORAGE_ACTIVE_INDEX_LEAF_PAGE_ENTRY_LIMIT`
    is reached.

## Design

Raise `MYLITE_STORAGE_ACTIVE_INDEX_LEAF_PAGE_ENTRY_LIMIT` from `1024` to `2048`.

This keeps the active statement cache bounded while matching the current
prepared-insert branch-planning working set more closely. The cache already has
metadata validation on lookup and is refreshed by active leaf-page writes, so
the behavior change is only retention capacity.

## Compatibility Impact

No SQL, public API, storage-routing, metadata, or file-format behavior changes.
The cache remains transient active-statement state.

## Single-File And Embedded Lifecycle

No durable layout or companion file changes. Statement cleanup, rollback,
catalog invalidation, and active cache clearing still free the cache through the
existing lifecycle hooks.

## Binary-Size, License, And Dependency Impact

No dependency or license changes. Binary-size impact is negligible. Runtime
memory ceiling for this active cache doubles from roughly `4 MiB` of page bytes
to roughly `8 MiB` of page bytes, plus cache-entry metadata, per active cache
owner.

## Test And Verification Plan

- Keep existing active leaf-page cache hook coverage passing; those tests use
  the limit constant and should continue to verify append/recycle behavior.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Active index leaf-page cache tests still pass with the larger bounded limit.
- Prepared insert component benchmark records fewer branch leaf-range plan reads
  and level-two branch leaf plan reads.
- No new durable state, public behavior, dependency, or unbounded memory growth
  is introduced.

## Verification

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - Passed in `143.89 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - Passed in `168.64 sec`.
- `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  - Prepared insert step component: `49.500 us/op`.
  - Branch leaf-range plan reads: `896`.
  - Branch refold entryset reads: `29`.
  - Branch refold entryset cache hits: `1111`.
  - Level-two branch leaf plan reads: `210`.

The preceding slice's comparable benchmark reported `1458` branch leaf-range
plan reads and `218` level-two branch leaf plan reads with the smaller cache
limit.

## Risks

- The larger full-page cache increases transient memory use. Keeping the bound
  at `2048` avoids an unbounded cache and is still much smaller than the
  multi-hundred-megabyte temporary write footprint already exercised by this
  benchmark.

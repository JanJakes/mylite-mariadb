# Branch Refold Cache Leaf Split Maintenance

## Problem

The 100k prepared-insert component sample after the branch refold capacity
precheck still reports fitting refold entryset rebuilds:

- branch refold entryset reads: `471`
- branch refold entryset cache hits: `669`

The active branch-refold entryset cache is preserved across simple branch-leaf
inserts and leaf-range redistribution, but single-level leaf splits still remove
the cache even though they do not change the logical index entryset. A split
only redistributes the selected leaf's existing entries plus the inserted row
across two child leaves while keeping the same root page and level.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB insert execution still reaches MyLite through:
  - `mariadb/sql/sql_insert.cc:Write_record::single_insert()`
  - `mariadb/sql/handler.cc:handler::ha_write_row()`
  - `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()`
- MyLite refs:
  - `packages/mylite-storage/src/storage.c:update_branch_refold_entryset_caches_after_branch_insert()`
    removes branch-refold caches for structural branch inserts unless
    `branch_insert_preserves_refold_entryset_cache()` accepts the plan.
  - `packages/mylite-storage/src/storage.c:split_branch_index_leaf_entry()`
    rewrites the selected leaf, appends one new leaf, and refreshes the same
    level-`1` branch root.
  - `packages/mylite-storage/src/storage.c:update_branch_refold_entryset_cache_after_simple_branch_insert()`
    already maintains the cached logical entryset by inserting the new
    `(key, row_id)` in sorted order.

## Design

Allow branch-refold cache preservation for single-level `split_leaf` plans when
the root stays level-`1`:

- keep excluding root promotion, lower/upper branch splits, refold rewrites,
  and deeper branch inserts;
- require the plan to target the same root and a concrete leaf page;
- use the existing sorted logical insert helper to add the new `(key, row_id)`
  to the cached entryset;
- keep removing the cache if the entry is absent, metadata no longer matches,
  or the logical insert cannot be applied.

This treats a leaf split like the logical insert it is for cache purposes. The
physical child list changes, but the cache key is logical and already validates
root page id, table id, index number, level, key size, and total entry count.

## Compatibility Impact

No SQL, public API, storage-routing, metadata, or file-format behavior changes.
The cache remains an opportunistic active-statement optimization and can always
be rebuilt from branch leaves.

## Single-File And Embedded Lifecycle

No companion files or durable layout changes. Rollback, catalog invalidation,
and statement cleanup continue to clear active branch-refold caches through the
existing lifecycle hooks.

## Binary-Size, License, And Dependency Impact

No dependency or license changes. The slice changes one first-party cache
preservation predicate and extends existing hook coverage.

## Test And Verification Plan

- Extend the branch-refold cache hook so a `split_leaf` branch insert preserves
  the cached sorted logical entryset.
- Keep the existing redistribution preservation, stale entry-count miss, table
  invalidation, and sorted-cache publication coverage passing.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Single-level leaf splits preserve an existing branch-refold entryset cache by
  inserting the new logical row.
- Root promotions and deeper structural branch changes still remove or replace
  caches.
- Existing branch insert, split, refold, rollback, and routed storage tests
  pass.
- The prepared insert component benchmark records updated refold counters and
  timing.

## Verification

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - Passed in `133.84 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - Passed in `178.93 sec`.
- `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  - Prepared insert step component: `69.392 us/op`.
  - Branch leaf-range plan reads: `1458`.
  - Branch refold entryset reads: `471`.
  - Branch refold entryset cache hits: `669`.
  - Level-two branch leaf plan reads: `218`.

The prepared-insert sample did not hit this newly preserved split-leaf path
often enough to move the headline refold counters. The slice is still covered
as a storage invariant, and the next performance target should focus on the
remaining fitting refold reads visible in the sample.

## Risks

- Preserving across a structural change could be unsafe if the root level or
  logical entry count changes unexpectedly. The predicate remains limited to
  same-root leaf splits, and cache lookup still requires matching level and
  incremented entry count before reuse.

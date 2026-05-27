# Branch Refold Insert Retarget Preservation

## Problem

The 100k prepared-insert component sample still rebuilds fitting branch-refold
entrysets after earlier cache-maintenance slices:

- branch refold entryset reads: `471`
- branch refold entryset cache hits: `669`

Local test-hook diagnostics showed those reads are fitting refold plans, and
most misses happen with an empty active branch-refold entryset cache. Temporarily
skipping the insert path's broad table-mutation refold-cache clear reduced the
same sample to `3` branch refold entryset reads and `1156` cache hits, so the
remaining cost is cache lifetime rather than refold capacity.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB insert execution still reaches MyLite through:
  - `mariadb/sql/sql_insert.cc:Write_record::single_insert()`
  - `mariadb/sql/handler.cc:handler::ha_write_row()`
  - `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()`
- MyLite refs:
  - `packages/mylite-storage/src/storage.c:mylite_storage_append_row_with_index_entries()`
    calls `retarget_durable_caches_after_table_mutation_in_statement()` after
    writing a row, then calls
    `update_branch_refold_entryset_caches_after_branch_insert()`.
  - `retarget_durable_caches_after_table_mutation_in_statement()` clears every
    active branch-refold entryset cache for the table. That is required for
    generic update/delete mutation, but it is too broad for planned inserts.
  - `update_branch_refold_entryset_caches_after_branch_insert()` already has
    the maintained-index insert plan and can invalidate or maintain the exact
    branch roots affected by the insert.

## Design

Use an insert-specific durable-cache retarget path that defers durable cache
retargeting without clearing branch-refold entryset caches for the whole table.
Then make insert-plan cache maintenance explicitly handle all stale cases:

- fallback index-entry writes remove branch-refold caches for the affected
  table/index number;
- single-page maintained-root inserts remove the matching root cache;
- deep branch inserts remove their matching root cache;
- refold branch inserts publish the planning-built post-refold entryset;
- simple, redistribution, and same-root leaf-split branch inserts preserve the
  cache with a sorted logical insert;
- unsupported structural branch changes still remove the matching root cache.

Update/delete paths keep the existing broad table clear because they can move,
remove, or rewrite arbitrary index entries.

## Compatibility Impact

No SQL, public API, storage-routing, metadata, or file-format behavior changes.
The change only preserves transient active-statement planning state across
inserts when the maintained-index plan can update or invalidate it precisely.

## Single-File And Embedded Lifecycle

No companion files or durable layout changes. Statement cleanup, rollback,
catalog invalidation, update, and delete still clear branch-refold caches through
the existing lifecycle hooks.

## Binary-Size, License, And Dependency Impact

No dependency or license changes. The slice adds one insert-specific cache
retarget helper and one precise table/index cache invalidation helper.

## Test And Verification Plan

- Add storage hook coverage proving insert retargeting can preserve a matching
  branch-refold cache while a fallback index-entry write invalidates only the
  affected table/index cache.
- Keep the existing branch-refold cache roundtrip, capacity precheck, branch
  insert, refold, rollback, update, delete, and routed storage tests passing.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Insert-table cache retargeting no longer clears all active branch-refold
  entryset caches for the table.
- Fallback index-entry writes still invalidate branch-refold caches for their
  affected table/index number.
- Update/delete and broad catalog invalidation still clear branch-refold caches.
- Prepared insert component benchmark records materially fewer branch refold
  entryset reads without changing SQL-visible behavior.

## Verification

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - Passed in `133.84 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - Passed in `202.64 sec`.
- `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  - Prepared insert step component: `52.811 us/op`.
  - Branch leaf-range plan reads: `1458`.
  - Branch refold entryset reads: `29`.
  - Branch refold entryset cache hits: `1111`.
  - Level-two branch leaf plan reads: `218`.

The same benchmark before this slice reported `471` branch refold entryset
reads and `669` cache hits. The remaining `29` reads are the conservative cost
of cold caches and fallback index-entry invalidations.

## Risks

- Preserving a cache across an insert fallback would hide live append-tail
  entries from a later refold. The design avoids that by invalidating caches for
  `index_entry_changed` entries before maintaining branch-plan caches.

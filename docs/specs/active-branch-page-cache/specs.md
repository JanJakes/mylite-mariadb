# Active Branch Page Cache

## Problem

Prepared insert profiling after active leaf-page caching moved level-`2`
selected leaves out of the hot path still shows branch planning work in
`plan_maintained_index_root_inserts()` and
`plan_level_two_branch_index_root_insert()`. The root branch page and selected
level-`1` child branch page are stable within the current active statement
unless the statement rewrites them, but repeated prepared inserts still reread
or revalidate those branch pages.

## Source Findings

- Base line: MariaDB 11.8.6, commit
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- MariaDB refs:
  - `mariadb/sql/sql_insert.cc:Write_record::single_insert()` executes a row
    insert through `handler::ha_write_row()`.
  - `mariadb/sql/handler.cc:handler::ha_write_row()` delegates the row write
    to the engine `write_row()` method.
  - `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()` routes durable
    MyLite inserts to `mylite_storage_append_row_with_index_entries()`.
- MyLite refs:
  - `packages/mylite-storage/src/storage.c:plan_maintained_index_root_inserts()`
    reads each maintained index root before dispatching branch-root planning.
  - `packages/mylite-storage/src/storage.c:plan_level_two_branch_index_root_insert()`
    reads and decodes the selected child branch page before descending to the
    selected leaf.
  - `packages/mylite-storage/src/storage.c:pager_write_page()` and
    `pager_write_maintained_insert_page()` publish or buffer branch-page
    rewrites during maintained index execution.

## Design

- Add a bounded active statement-owned branch-page cache on the root active
  cache owner, matching the active leaf-page cache ownership model.
- Cache only decoded branch pages. Each entry stores a full page copy plus the
  decoded branch metadata needed to rebuild `mylite_storage_index_branch_page`
  without I/O or checksum validation.
- Use the active branch-page cache in maintained index insert planning for:
  - branch root pages read by `plan_maintained_index_root_inserts()`;
  - level-`2` selected child branch pages read by
    `plan_level_two_branch_index_root_insert()`.
- Refresh active branch-page cache entries from pager branch-page writes and
  buffered maintained root/branch writes. This keeps same-statement planning
  current after fence, child-count, split, and promotion rewrites.
- Clear active branch-page caches with active leaf-page caches on nested
  rollback and catalog-root invalidation.

The slice deliberately does not add a durable branch-page cache and does not
change deeper level-`3+` branch descent yet. Those can follow once level-`2`
planning proves the ownership and invalidation rules.

## Compatibility Impact

No SQL, public API, storage-routing, metadata, or file-format behavior changes.
The cache only removes repeated in-memory planning reads inside the current
embedded storage statement.

## DDL Metadata Routing Impact

None. Catalog publication and table-definition routing are unchanged.

## Single-File And Embedded Lifecycle

The cache is transient statement-local memory. It does not change the primary
`.mylite` file layout and does not add companion files. Rollback and catalog
invalidation clear cached branch pages before stale branch fences can be reused.

## Public API Or File-Format Impact

None.

## Storage-Engine Routing Impact

No routing-policy change. Routed `InnoDB`, `MyISAM`, `Aria`, explicit MyLite,
and supported aliases continue to use the same MyLite storage path.

## Wire-Protocol Or Integration Impact

None.

## Binary-Size, License, And Dependency Impact

No new dependency or license impact. Binary-size impact is limited to the
active cache and test hooks.

## Test And Verification Plan

- Extend multi-level branch storage coverage so two same-statement inserts
  below a level-`2` branch root prove that the first planning pass reads the
  root and child branch pages, while the second reuses the active branch-page
  cache refreshed by the first insert's pager writes.
- Keep existing active leaf cache, branch split, rollback, and routed storage
  smoke coverage passing.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`

## Verification

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`: passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `162.59 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`: passed.
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`: passed
  in `31.85 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`: passed,
  with prepared insert step at `82.360 us/op`.

## Acceptance Criteria

- Level-`2` branch insert planning reuses active cached root and child branch
  pages inside one storage statement.
- Pager and buffered maintained branch writes refresh matching active branch
  cache entries.
- Nested rollback and catalog-root invalidation clear active branch-page caches.
- Existing maintained-index correctness and rollback coverage still passes.
- Prepared insert component profiling shows reduced branch decode/checksum work
  or records the next bottleneck.

## Risks

- A cached branch page not refreshed after a fence or child-count rewrite could
  route a later insert through stale metadata. The cache therefore refreshes
  from both direct pager writes and buffered maintained branch writes.
- The bounded cache may evict broad workloads. That is acceptable for this
  slice because eviction only loses the optimization; correctness comes from
  rereading and redecoding the page.

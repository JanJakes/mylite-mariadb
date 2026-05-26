# Active Branch Leaf Page Cache

## Problem

Prepared insert profiling after branch-tail overlay cache ownership moved to the
root active cache owner shows the next hot path under branch leaf planning:
`plan_branch_index_root_insert()`,
`try_plan_branch_leaf_range_insert_redistribution()`,
`read_branch_leaf_range_plan_scan_leaf()`, and
`decode_index_leaf_page()`.

The existing durable index leaf page cache is intentionally skipped while active
statements or read snapshots are in scope. That is correct for committed
read-side caching, but it means repeated prepared inserts in one active
checkpoint repeatedly reread and revalidate the same branch leaf siblings even
when the storage statement already owns the current page view.

## Source Findings

- Base line: MariaDB 11.8.6, commit
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- MariaDB refs:
  - `mariadb/sql/sql_insert.cc:Write_record::single_insert()` executes accepted
    row inserts through the handler.
  - `mariadb/sql/handler.cc:handler::ha_write_row()` calls the engine
    `write_row()` implementation.
  - `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()` routes durable
    MyLite inserts to `mylite_storage_append_row_with_index_entries()`.
- MyLite refs:
  - `packages/mylite-storage/src/storage.c:plan_branch_index_root_insert()`
    reads and decodes the selected branch leaf before deciding whether to
    insert in place, redistribute, split, or refold.
  - `packages/mylite-storage/src/storage.c:read_branch_leaf_range_plan_scan_leaf()`
    reads sibling leaves and decodes their entry counts while planning
    redistribution.
  - `packages/mylite-storage/src/storage.c:read_cached_durable_index_leaf_page()`
    serves decoded durable leaf pages only outside active statements and
    snapshots.
  - `packages/mylite-storage/src/storage.c:pager_write_page()` and
    `pager_write_maintained_insert_page()` are the branch-maintenance write
    paths for updated and newly split leaf pages.

## Design

- Add an active statement-owned leaf-page cache on the root active cache owner,
  matching exact-index and branch-tail overlay cache ownership.
- Populate the cache only from decoded leaf pages or leaf pages written through
  the pager. Cached entries include a full page copy and decoded metadata, so
  later reads can rebuild the `mylite_storage_index_leaf_page` view without
  redoing page I/O or checksum validation.
- Use the active cache in branch insert planning for the selected leaf and
  redistribution sibling leaves, including selected leaf descent below
  level-`2` branch roots.
- Update active cache entries when the pager writes leaf pages. This keeps
  same-statement planning current after in-place insert, redistribution, and
  split writes.
- Decode pager-written leaf pages against the active statement's visible page
  range, including buffered append pages, so leaf entries for rows inserted
  before header publication can refresh the cache. If a pager-written leaf
  cannot be decoded, clear the active leaf cache instead of keeping a stale
  planning entry.
- Clear active leaf-page caches with other active planning caches on nested
  rollback and catalog-root invalidation.

The slice deliberately does not use the active cache for read-only handler
cursor construction or durable reads. Those paths already have committed-state
cache rules and different snapshot constraints.

## Compatibility Impact

No SQL, public API, storage-routing, metadata, or file-format behavior changes.
The cache only reduces repeated in-memory planning work inside the current
embedded storage statement.

## DDL Metadata Routing Impact

None. Catalog publication and table-definition routing are unchanged.

## Single-File And Embedded Lifecycle

The cache is transient statement-local memory. It does not change primary
`.mylite` file layout and does not add companion files. Nested rollback clears
parent active leaf-page caches conservatively because nested work may have
updated root-owned entries before rollback.

## Public API Or File-Format Impact

None.

## Storage-Engine Routing Impact

No routing-policy change. Routed `InnoDB`, `MyISAM`, `Aria`, explicit MyLite,
and supported aliases continue to use the same MyLite storage path.

## Wire-Protocol Or Integration Impact

None.

## Binary-Size, License, And Dependency Impact

No new dependency or license impact. Binary-size impact is limited to a small
active cache and test hooks.

## Test And Verification Plan

- Add storage coverage where two same-statement inserts target a full branch
  leaf with a slack right sibling. The first insert may read the sibling while
  planning redistribution; the second should reuse the active cache and perform
  zero uncached branch leaf range planning reads.
- Add storage coverage where repeated same-statement appends fill and split a
  branch leaf whose new row ids are beyond the pre-publish header page count.
  This verifies pager-written leaves refresh the active cache instead of leaving
  stale entry counts behind.
- Extend multi-level branch storage coverage so two same-statement inserts
  below a level-`2` branch root prove that the first selected-leaf descent may
  read the leaf, while the second reuses the active leaf cache refreshed by the
  first insert's pager writes.
- Keep existing rollback, split, redistribution, and branch-tail overlay tests
  passing.
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
  in `153.96 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`: passed.
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`: passed
  in `33.27 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`: passed,
  with prepared insert step at `83.288 us/op`.

## Acceptance Criteria

- Branch insert planning can reuse active cached leaf pages.
- Pager leaf writes refresh matching active cache entries.
- Level-`2` branch insert planning reuses cached selected leaf pages instead of
  re-reading and re-checksumming leaves rewritten earlier in the same statement.
- Nested rollback clears parent active leaf-page caches.
- Existing maintained-index correctness and rollback coverage still passes.
- Prepared insert component profiling moves remaining cost away from repeated
  branch sibling leaf decode/checksum work or records the next bottleneck.

## Risks

- A cache entry that is not refreshed after a branch leaf rewrite could cause
  stale entry counts and incorrect planning. The cache therefore updates from
  the pager write path rather than relying on plan-time guesses.
- Nested rollback can invalidate root-owned entries written by a child
  statement; parent-chain clearing handles that conservatively.

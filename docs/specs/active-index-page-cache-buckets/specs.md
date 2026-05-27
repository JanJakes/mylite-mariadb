# Active Index Page Cache Buckets

## Problem

After the recent branch-planning read reductions, a five-second sampled
prepared-insert component run showed visible CPU in active index leaf and branch
page cache lookup:

- `read_cached_active_index_leaf_page()` spent samples under
  `find_index_leaf_page_cache_entry()`.
- `read_index_branch_child_page()` spent smaller but similar samples under
  active branch-page cache lookup.
- The same sample also showed `pwrite()` from append-buffer flushes as the
  larger remaining cost, but that is write-volume-bound and belongs to row/index
  layout work rather than another cache-policy tweak.

The active index page caches already have a last-hit index and high-water miss
guard, but lookups for retained non-last pages still scan up to the bounded
cache size.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB insert execution still reaches MyLite through:
  - `mariadb/sql/sql_insert.cc:Write_record::single_insert()`
  - `mariadb/sql/handler.cc:handler::ha_write_row()`
  - `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()`
- MyLite refs:
  - `packages/mylite-storage/src/storage.c:read_cached_active_index_leaf_page()`
    and `read_cached_active_index_branch_page()` query statement-owned page
    caches during branch planning.
  - `packages/mylite-storage/src/storage.c:find_index_leaf_page_cache_entry()`
    and `find_index_branch_page_cache_entry()` use a last-hit fast path before
    a linear scan.
  - `packages/mylite-storage/src/storage.c:recycle_index_leaf_page_cache_entry()`
    and `recycle_index_branch_page_cache_entry()` reuse fixed cache slots once
    active cache limits are reached.

## Design

Add transient page-id hash buckets to active and durable index leaf-page cache
sets and active branch-page cache sets:

1. Keep the existing last-hit and max-page-id checks first.
2. Maintain bucket heads and per-entry next links keyed by page id.
3. Rebuild buckets when cache entry capacity grows.
4. On append, link the new entry into its bucket.
5. On active-cache recycle, unlink the old page id from its bucket before
   assigning the replacement page id, then link the replacement.
6. Fall back to the existing linear scan if a cache has no bucket table.

This keeps cache entries and invalidation ownership unchanged; the buckets are
only a lookup index over already-owned cache entries.

## Compatibility Impact

No SQL, public API, storage-engine routing, metadata, or file-format behavior
changes. Cache misses still fall back to the same durable or pager reads, and
cache hits still return only entries whose page id matches the requested page.

## Single-File And Embedded Lifecycle

No durable state, companion files, recovery behavior, or open/close lifecycle
changes. The bucket arrays are transient statement-local or durable-read-cache
memory and are freed with their owning caches.

## Binary-Size, License, And Dependency Impact

No dependency or license changes. The slice adds small first-party hash-bucket
bookkeeping in `storage.c`.

## Test And Verification Plan

- Strengthen active leaf/branch page cache hook coverage so recycled old page
  ids are not found through the lookup path while replacement page ids are.
- Keep storage and embedded storage-engine smoke tests passing.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Existing last-hit behavior and max-page-id miss behavior remain intact.
- Active cache recycle updates the bucket index and cannot return a stale page
  id for a reused slot.
- Cache lookup no longer needs a linear scan for ordinary bucketed hits/misses.
- Existing storage and routed storage-engine tests pass.
- The prepared insert component benchmark records the updated local result or
  confirms write volume is the next dominant bottleneck.

## Verification

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `./build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `137.78 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `192.78 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed, with prepared insert step at `56.441 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components --profile-iterations=1000000 1000`:
  passed, with prepared insert step at `17.993 us/op`.

The 100k run remained dominated by I/O noise and preserved the same storage
counters: branch leaf-range plan reads `85`, branch refold entryset reads/cache
hits `29` / `1111`, level-two branch leaf plan reads `102`, active branch page
plan reads `0`, branch insert writer branch/leaf decodes `0` / `0`, and branch
tail overlay scans/read pages `2` / `48`.

A five-second sample of the one-million-iteration run no longer showed
`find_index_leaf_page_cache_entry()` and showed only one sample in
`find_index_branch_page_cache_entry()`. The remaining sampled work was dominated
by `pwrite()` under `flush_statement_append_page_buffer()`, confirming the next
large insert-performance step is reducing row/index write volume rather than
another cache lookup pass.

## Risks

- Incorrect bucket unlinking during recycle could make a valid retained entry
  unreachable or leave a stale mapping. The strengthened storage hook checks
  both stale-id misses and replacement-id hits after leaf and branch cache
  recycling.
- Bucket memory grows with cache capacity. The active cache limits remain
  unchanged, so the extra memory is bounded and cleared with the cache.

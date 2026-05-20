# Exact Index Cache Row-Id Buckets

## Problem

The active exact-index cache now keeps key lookup buckets valid across row
replacement and delete maintenance, but row-id invalidation still scans every
cached entry. Current storage-smoke update profiling shows
`remove_exact_index_cache_entries_by_row_id()` as the largest remaining
MyLite-local cache maintenance cost after physical append flushing.

This slice adds a separate transient row-id bucket index so successful updates
and deletes can tombstone matching exact-index entries without walking the full
cache.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc:8373-8425` calls the storage engine's
  `update_row()` implementation after in-server constraint checks.
- `mariadb/storage/mylite/ha_mylite.cc:2623-2770` prepares replacement row
  payloads and index entries, checks duplicate/FK behavior, then calls
  `mylite_storage_update_row_with_index_entries()` for durable MyLite tables.
- `packages/mylite-storage/src/storage.c:4483-4624` validates the source row,
  appends replacement row/state/index-entry pages, publishes the checkpoint,
  then maintains active exact-index caches by source and replacement row id.
- `replace_active_exact_index_cache_entries()` calls
  `remove_exact_index_cache_entries_by_row_id()` for every active exact-index
  cache on the same table.
- Existing exact-index cache key buckets are keyed by byte-string index keys
  and use one `bucket_next` chain per entry. Row-id buckets need their own next
  links because the same cache entry must be reachable through both key lookup
  and row-id invalidation paths.

## Design

- Add a separate row-id bucket index to `mylite_storage_exact_index_cache`.
- Keep key lookup buckets unchanged.
- Build row-id buckets lazily on the first row-id removal when the cache has
  live entries.
- Keep row-id buckets valid across appends and removals when capacity permits:
  - append links the new entry into both key buckets and row-id buckets when
    those indexes are valid;
  - removal walks only the row-id bucket, unlinks matching live entries from
    row-id and key chains, and tombstones them;
  - compaction rebuilds whichever bucket indexes were valid before compaction.
- If row-id bucket allocation fails, fall back to the existing full scan and
  leave row-id buckets invalid. Cache acceleration remains best-effort.
- Preserve live entry order for exact-entryset reads and durable cache
  promotion by keeping tombstones until compaction, as the current key-bucket
  maintenance already does.

## Affected Subsystems

- MyLite storage active exact-index cache maintenance.
- Durable exact-index cache copies and promotion, because the cache struct owns
  the new transient arrays.
- Large transaction update/delete performance over cached exact indexes.

## Compatibility Impact

SQL behavior does not change. The cache is a transient acceleration structure
over already-visible MyLite index entries. Requested `MYLITE`, omitted/default,
`InnoDB`, `MyISAM`, and `Aria` routed tables keep the same storage semantics.

## Single-File And Lifecycle Impact

No durable file-format, journal, WAL, lock, or companion-file change. The new
state is process-local memory attached to an active or durable cache and is
released with the cache.

## Public API And File-Format Impact

No public API or file-format change.

## Binary-Size And Dependency Impact

Small first-party C change. No new dependency.

## Tests And Verification

- Extend storage coverage for active exact-index cache replacement/deletion to
  force row-id bucket construction, repeated replacement, deletion, compaction,
  and post-commit promotion.
- Run the storage-smoke build targets, storage unit test, focused embedded
  storage-engine coverage, and the full storage-smoke CTest suite.
- Run an update performance baseline sample.
- Run `git diff --check` and `git clang-format --diff`.

## Verification Results

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `ctest --test-dir build/storage-smoke-dev --output-on-failure`
  - 10/10 tests passed, total real time `49.99 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=updates 1000 100000`
  - direct primary-key updates: `16.439 us/op`.
  - prepared primary-key updates: `10.386 us/op`.
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`

## Acceptance Criteria

- Row-id removals use bucket lookup when row-id buckets are available.
- Key lookups and exact-entryset reads stay correct after many replacement and
  delete maintenance operations.
- Cache compaction preserves live entry order and rebuilds valid bucket indexes.
- Existing storage and embedded storage-engine tests remain green.

## Risks And Open Questions

- Row-id buckets add one more per-cache transient allocation. This is acceptable
  for the current performance profile, but a future memory-tuned profile may
  want cache sizing policy around both key and row-id indexes.
- The update path remains dominated by physical append volume after cache
  maintenance. SQLite-like performance still needs a pager/WAL or replacement
  coalescing design.

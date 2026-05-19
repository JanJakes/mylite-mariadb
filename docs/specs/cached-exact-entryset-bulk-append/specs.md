# Cached Exact Entryset Bulk Append

## Problem

After read-session and checkpoint-snapshot caching, repeated exact secondary
reads still spend visible time materializing cached durable exact-index matches.
The sampled path is:

- `ha_mylite::build_index_cursor()`
- `mylite_storage_read_exact_index_entries()`
- `append_cached_durable_exact_index_entries()`
- `append_index_entry_to_entryset()`
- `grow_index_entryset_for_append()`

The durable exact-index cache already avoids rescanning append-only index
pages, but exact reads with many duplicate secondary-key values still append
each matching cached entry separately. That grows the result key, offset, size,
and row-id arrays with repeated `realloc()` calls.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::build_index_cursor()` builds handler
  cursors from `mylite_storage_read_exact_index_entries()` for full-key exact
  secondary predicates.
- `packages/mylite-storage/src/storage.c::mylite_storage_read_exact_index_entries()`
  uses published leaf roots first, then falls back to the durable exact-index
  read cache when no leaf root applies.
- `append_cached_durable_exact_index_entries()` iterates cached entries and
  calls `append_index_entry_to_entryset()` for every matching key.
- `append_index_leaf_matches_to_entryset()` already bulk-grows the same public
  entryset representation after counting matching leaf entries.
- The local sample after discovery read sessions shows allocator-heavy frames
  below `append_cached_durable_exact_index_entries()` during the secondary
  exact-select benchmark.

## Design

- Keep `mylite_storage_index_entryset` unchanged. It is a public C struct and
  should not gain internal capacity fields for this transient optimization.
- Add an internal helper that appends all matching entries from one
  `mylite_storage_exact_index_cache` by:
  - counting matching key images,
  - checking `SIZE_MAX` overflow before multiplying by fixed key size,
  - growing the entryset once,
  - copying matching key images, key offsets, key sizes, and row ids in cache
    order.
- Replace the per-entry append loop in
  `append_cached_durable_exact_index_entries()` with that helper.
- Preserve existing cache eligibility. Active statements and read snapshots
  still bypass the durable cache, and published leaf-root reads keep using the
  leaf path.

## Compatibility Impact

SQL-visible behavior is unchanged. The same keys and row ids are returned in
the same order for the same durable header state.

## Single-File And Lifecycle Impact

No file-format change and no new companion files. This is an in-memory result
construction optimization for storage API calls.

## Public API And File-Format Impact

No public API or file-format change.

## Storage-Engine Routing Impact

Durable MyLite-routed exact secondary reads benefit when the durable exact
index cache is used. Volatile MEMORY/HEAP paths and published leaf-root exact
reads are unchanged.

## Binary-Size And Dependency Impact

No new dependency. Binary impact is limited to one small internal helper.

## Test And Verification Plan

- Add focused storage coverage for repeated exact secondary entryset reads with
  many duplicate key entries so the durable cache materialization path returns
  the complete ordered entryset.
- Run storage unit tests.
- Rebuild the storage-smoke target and run the storage-engine compatibility
  harness.
- Run the local performance baseline and compare secondary exact-select
  timings.
- Run changed-line formatting checks and `git diff --check`.

## Acceptance Criteria

- Cached durable exact-index entryset reads grow result arrays once per read
  instead of once per matching entry.
- Repeated many-match exact secondary reads return every matching row id in
  cache order.
- Storage and storage-engine compatibility tests pass.
- Secondary exact-select benchmark timings improve or remain within noise while
  preserving result checksums.

## Verification Results

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_embedded_storage_engine_test`
- `tools/mylite-compat-harness run storage-engine`
- `build/storage-smoke-dev/tools/mylite_perf_baseline 1000 10000`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `git diff --check`

Local perf sample after implementation:

- direct secondary exact selects: `104.368 us/op`
- prepared secondary exact selects: `75.936 us/op`
- direct published-leaf secondary exact selects: `98.568 us/op`
- prepared published-leaf secondary exact selects: `74.442 us/op`

## Risks

- This does not remove the larger per-cursor file-open/read-lock startup cost
  seen in the same sample. That needs a broader shared read-runtime or pager
  slice.
- The first cache load still scans append-only index entries. Maintained
  navigable indexes remain necessary for SQLite-like first-lookup cost.

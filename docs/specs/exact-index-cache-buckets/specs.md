# Exact Index Cache Buckets

## Problem

After read file-handle reuse, repeated exact secondary reads spend visible time
inside the durable exact-index cache itself:

- `mylite_storage_read_exact_index_entries()`
- `append_cached_durable_exact_index_entries()`
- `append_exact_index_cache_matches_to_entryset()`
- repeated `memcmp()` over cached key images

The durable exact-index cache avoids rereading append-only index pages, but it
still scans every cached entry for each exact key lookup. For benchmark data
with 1000 cached entries and 100 matching rows per secondary value, every exact
read scans all 1000 entries twice: once to count matches and once to copy them.
Primary-key point reads have the same linear cache probe shape, just with one
matching row.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::build_index_cursor()` calls storage
  exact lookup APIs for full-key raw equality cursors.
- `packages/mylite-storage/src/storage.c::mylite_storage_find_index_entry()`
  uses active and durable exact-index caches for single-row probes.
- `packages/mylite-storage/src/storage.c::mylite_storage_read_exact_index_entries()`
  uses durable exact-index caches when no published leaf root handles the
  lookup.
- `mylite_storage_exact_index_cache` stores fixed-width key images and row ids
  in append order, with active cache maintenance removing or appending entries
  as row updates and deletes publish new row state.
- The sample after read file-cache reuse shows
  `append_exact_index_cache_matches_to_entryset()` and `memcmp()` as the largest
  MyLite storage-local exact-index entryset cost.

## Design

- Add an internal, lazily rebuilt bucket index to
  `mylite_storage_exact_index_cache`.
- Keep the public storage API and file format unchanged.
- Bucket state stores:
  - bucket heads,
  - per-entry next links,
  - bucket count,
  - validity flag.
- Appending or removing cache entries marks the bucket index invalid.
- Lookup paths call `ensure_exact_index_cache_buckets()` before probing. The
  builder chooses a power-of-two bucket count at least twice the entry count,
  then links entries in original cache order by iterating backward while
  prepending into each bucket.
- Exact single-entry and entryset lookups hash the requested key and walk only
  that bucket, preserving the previous cache order for ties.
- The existing bulk entryset grow remains in place after the matching bucket
  entries are counted.

## Compatibility Impact

SQL-visible behavior is unchanged. Matching entries remain in the same order as
the prior linear cache scan.

## Single-File And Lifecycle Impact

No file-format change and no new companion files. The bucket index is transient
process memory attached to an already transient exact-index cache.

## Public API And File-Format Impact

No public API or file-format change.

## Storage-Engine Routing Impact

Durable routed tables benefit through existing exact-index cache paths. Active
statement duplicate checks also use the bucket index after their cache is built
or maintained.

## Binary-Size And Dependency Impact

No new dependency. The implementation adds small internal arrays and a
byte-key hash helper.

## Test And Verification Plan

- Extend the many-duplicate exact-entryset storage test to exercise cache-hit
  ordering after bucket construction.
- Rely on existing update/delete/truncate coverage to catch stale active or
  durable cache results after bucket invalidation.
- Run storage unit tests.
- Rebuild storage-smoke targets and run the storage-engine compatibility
  harness.
- Run the local performance baseline and compare primary-key and secondary
  exact-select timings.
- Run changed-line formatting checks and `git diff --check`.

## Acceptance Criteria

- Exact cache entryset lookups avoid scanning every cached entry for every
  repeated key.
- Single-row exact cache lookups preserve first-match behavior.
- Cache append/remove operations invalidate bucket state.
- Storage and storage-engine compatibility tests pass.

## Verification Results

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_embedded_storage_engine_test`
- `tools/mylite-compat-harness run storage-engine`
- `build/storage-smoke-dev/tools/mylite_perf_baseline 1000 10000`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `git diff --check`

Local perf sample after implementation, second run:

- direct primary-key point selects: `41.861 us/op`
- prepared primary-key point selects: `20.864 us/op`
- direct secondary exact selects: `79.012 us/op`
- prepared secondary exact selects: `51.550 us/op`
- direct published-leaf secondary exact selects: `78.733 us/op`
- prepared published-leaf secondary exact selects: `52.501 us/op`

## Risks

- Hash buckets reduce repeated lookup cost but add memory proportional to cache
  entry count. The existing cache-count limits remain the outer bound.
- This does not address per-read-statement journal `stat()` checks, advisory
  `flock()`, SQL optimizer/filesort overhead, or the need for maintained
  navigable index pages.

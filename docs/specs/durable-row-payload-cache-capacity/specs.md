# Durable Row Payload Cache Capacity

## Problem

The storage point-lookup benchmark shows a meaningful gap between exact
primary-key entry lookup and full row lookup. For the current 10000-row
benchmark, entry lookup measures the exact-index/read-scope path while row
lookup also materializes the row payload.

The durable row-payload cache currently keeps up to 4096 entries per cache.
The benchmark cycles through 10000 distinct fixed-size rows, so the cache churns
before the workload can become a steady-state cached row-materialization loop.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `packages/mylite-storage/src/storage.c::read_indexed_row_payload_from_open_file()`
  checks active and durable row-payload caches before reading row pages.
- `MYLITE_STORAGE_DURABLE_ROW_PAYLOAD_ENTRY_LIMIT` caps retained durable row
  payloads at 4096 entries.
- `tools/mylite_perf_baseline --phase=storage-pk-entry-lookups 10000 1000000`
  measures storage exact-index lookup without row payload materialization.
- `tools/mylite_perf_baseline --phase=storage-pk-row-lookups 10000 1000000`
  measures the same lookup plus row payload materialization and copy-out.

## Design

- Increase `MYLITE_STORAGE_DURABLE_ROW_PAYLOAD_ENTRY_LIMIT` from `4096` to
  `16384`.
- Keep the number of durable row-payload cache sets unchanged.
- Keep active statement cache limits unchanged.
- Do not change row payload cache keys, invalidation, ownership, or row
  visibility rules.

## Compatibility Impact

No SQL-visible behavior should change. The cache stores already validated row
payload bytes and remains keyed by filename, catalog root/generation,
page-count identity, and table id.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, catalog, or companion-file lifecycle
change.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing-policy change. Durable routed tables can retain a larger hot row
payload working set. Volatile MEMORY/HEAP rows are unaffected.

## Binary-Size And Dependency Impact

No dependency or binary-size-sensitive source change. Runtime memory can grow
for durable row-payload caches when workloads touch more distinct row payloads.
For the current fixed-row benchmark shape, raising the cap from 4096 to 16384
allows the 10000-row hot set to remain cached.

## Tests And Verification

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build libmariadbd.a`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-pk-entry-lookups 10000 1000000`
  - `storage primary-key entry lookups`: `4.272 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-pk-row-lookups 10000 1000000`
  - `storage primary-key row lookups`: `4.831 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-selects 10000 1000000`
  - `prepared primary-key point selects`: `7.878 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-scalar-selects 10000 1000000`
  - `prepared scalar selects`: `0.735 us/op`

## Acceptance Criteria

- Durable row-payload cache capacity is large enough for the 10000-row local
  benchmark hot set.
- Existing cache invalidation and mutation retargeting behavior remains
  unchanged.
- Existing storage and storage-engine smoke tests pass.
- Benchmark evidence records the storage row-lookup effect and prepared
  point-select context.

## Risks And Unresolved Questions

- This is a bounded memory-for-speed tradeoff, not a replacement for pager or
  B-tree work.
- A future configurable cache budget may be better than fixed compile-time
  limits once workload targets are clearer.

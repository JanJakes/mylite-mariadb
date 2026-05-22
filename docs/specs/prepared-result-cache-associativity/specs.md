# Prepared Result Cache Associativity

## Problem

The one-row prepared result cache is direct-mapped with 4096 entries. That works
for tiny repeated point-read sets, but the local 10k-row prepared primary-key
benchmark cycles through more stable keys than the cache can retain. Sampling
`prepared-pk-select-components` showed repeated calls through
`ha_mylite::read_exact_unique_index_row_into()` and repeated one-row cache
publication, meaning the cache was thrashing instead of replaying rows inside
the retained read scope.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc::execute_cached_one_row_result()` replays
  cached rows only while the statement-owned prepared read scope remains open.
- `packages/libmylite/src/database.cc::store_one_row_result_cache_entry()` only
  publishes a cached row after MariaDB has produced one row and then returned
  `MYSQL_NO_DATA`.
- `packages/libmylite/src/database.cc::close_prepared_read_scope()` clears the
  cache, and writes close retained prepared read scopes before executing.
- The current cache uses a single slot per hashed key, so a stable working set
  larger than 4096 recurring keys can evict itself in access order.

## Design

- Keep the same conservative cache eligibility and invalidation rules.
- Keep 4096 hash sets, but store four ways per set.
- Probe all ways on lookup.
- On store, reuse an empty way or matching-key way first, then replace using a
  per-set round-robin cursor.
- Keep the cache statement-owned and bounded; no durable file-format, storage,
  handler, or SQL planner behavior changes.

## Compatibility Impact

No SQL-visible behavior change is intended. Cached rows are still populated only
from MariaDB-produced rows and replayed only inside the same retained read
scope. Unsupported SQL shapes, unsupported parameter kinds, no-row results, and
multi-row results continue through MariaDB.

## Single-File And Lifecycle Impact

No durable `.mylite` format, companion-file, recovery, journal, or locking
change. The cache is in-memory and cleared with the prepared read scope,
statement finalization, or any write that must invalidate a retained read
snapshot.

## Public API And File-Format Impact

No public API or file-format change.

## Storage-Engine Routing Impact

No routing policy change. The slice only improves the libmylite replay cache
above already-routed MyLite storage reads.

## Binary-Size And Dependency Impact

No dependency change. Runtime memory for cache-eligible statements grows from
4096 direct-mapped entries to 4096 four-way sets plus one byte of replacement
state per set. The cache is allocated only after a statement first publishes a
cacheable row.

## Test And Verification Plan

- Run `git diff --check`.
- Run `git clang-format --diff -- packages/libmylite/src/database.cc`.
- Build `mylite_embedded_statement_test`,
  `mylite_embedded_storage_engine_test`, and `mylite_perf_baseline`.
- Run focused embedded statement and storage-engine CTest coverage.
- Run the prepared primary-key component and ordinary point-select benchmark
  phases with a 10k-row working set.

## Verification Evidence

- `git diff --check`: passed.
- `git clang-format --diff -- packages/libmylite/src/database.cc`: passed.
- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_statement_test mylite_embedded_storage_engine_test
  mylite_perf_baseline`: passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'libmylite\.embedded-(statement|storage-engine)' --output-on-failure`:
  passed.
- `ctest --preset storage-smoke-dev --output-on-failure`: passed, 10/10.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-pk-select-components 10000 1000000`: row component
  `0.923 us/op`, done component `0.114 us/op`, reset component
  `0.029 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-pk-selects 10000 1000000`: prepared point selects
  `1.049 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-pk-select-reset-after-row 10000 1000000`: reset-after-row
  point selects `1.023 us/op`.

## Acceptance Criteria

- Existing prepared result-cache invalidation tests keep passing.
- The 10k-row prepared primary-key benchmark shows lower row/done component
  cost from fewer cache evictions.
- The diff remains limited to libmylite cache mechanics and roadmap/spec docs.

## Risks And Open Questions

- This remains a bounded replay cache, not a general MariaDB optimizer bypass.
  Cold point reads and working sets larger than the set-associative capacity
  still execute through MariaDB.
- Four ways materially reduce direct-mapped thrash without unbounded memory, but
  the final SQLite-like read profile still needs broader prepared execution and
  navigable storage-index work.

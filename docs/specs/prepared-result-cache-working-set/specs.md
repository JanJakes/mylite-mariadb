# Prepared Result Cache Working Set

## Problem

The four-way prepared one-row result cache still used 4096 hash sets. A 10k-row
recurring point-read working set can overload enough sets that every key in
those sets evicts another key before the next cycle. Sampling
`prepared-pk-select-components` after the step fast path still showed a
substantial MariaDB `mysql_stmt_execute()` bucket from cache misses.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc::find_one_row_result_cache_entry()` probes
  four ways in one hashed set.
- `packages/libmylite/src/database.cc::put_one_row_result_cache_entry()` uses a
  per-set round-robin replacement cursor when all ways are occupied.
- With 4096 sets, the 10k benchmark has enough keys per set that overloaded
  sets behave like recurring misses even though the total entry count is larger
  than the working set.

## Design

- Increase the cache set count from 4096 to 8192 while keeping four ways.
- Keep power-of-two static assertions and the same mask-based set lookup.
- Keep the existing cache eligibility, invalidation, replay, and replacement
  policy.
- Accept the larger statement-owned memory budget for now because current work
  is prioritizing read-path performance over size reduction.

## Compatibility Impact

No SQL-visible behavior change is intended. The cache still replays only
MariaDB-produced one-row results under the same retained prepared read scope.

## Single-File And Lifecycle Impact

No durable `.mylite` format, companion-file, locking, recovery, or storage
lifecycle change.

## Public API And File-Format Impact

No public API or file-format change.

## Storage-Engine Routing Impact

No routing policy change.

## Binary-Size And Dependency Impact

No dependency change. Cache-eligible prepared statements can allocate 8192
four-way entry slots plus replacement state after the first cacheable row.

## Test And Verification Plan

- Run `git diff --check`.
- Run `git clang-format --diff -- packages/libmylite/src/database.cc`.
- Build `mylite_embedded_statement_test`,
  `mylite_embedded_storage_engine_test`, and `mylite_perf_baseline`.
- Run focused embedded statement and storage-engine CTest coverage.
- Run the full storage-smoke CTest preset.
- Run prepared primary-key point-select benchmarks with a 10k-row working set.

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
  `0.232 us/op`, done component `0.030 us/op`, reset component
  `0.021 us/op` in the clean repeated run.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-pk-selects 10000 1000000`: prepared point selects
  `0.261 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-pk-select-reset-after-row 10000 1000000`: reset-after-row
  point selects `0.262 us/op`.

## Acceptance Criteria

- Prepared statement and storage-engine tests continue to pass.
- Prepared point-select benchmarks improve through fewer recurring cache
  misses.
- The cache remains bounded and invalidated by the existing prepared read-scope
  lifecycle.

## Risks And Open Questions

- This intentionally spends more per-statement memory. Size/profile work should
  revisit the final cache budget once the read path is closer to the target
  performance envelope.

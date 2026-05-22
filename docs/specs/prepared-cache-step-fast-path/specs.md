# Prepared Cache Step Fast Path

## Problem

After prepared result cache associativity and scalar value-copy work, cached
point reads still entered `mylite_step()` through the normal storage-context
setup. Cache-hit row replay and the following cached `DONE` result do not touch
the MyLite storage layer, but they still paid the per-step storage busy-timeout
and context scope cost.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc::mylite_step()` created
  `StorageBusyTimeoutScope` and `StorageContextScope` before checking whether
  a statement was complete or cache-served.
- `packages/libmylite/src/database.cc::execute_cached_one_row_result()` replays
  statement-owned cache entries without storage calls.
- `packages/libmylite/src/database.cc::fetch_statement_row()` handled the
  second step after a cache hit by returning `MYLITE_DONE` without fetching
  from MariaDB or storage.

## Design

- Return `MYLITE_DONE` before constructing storage scopes when the statement is
  already done.
- Finish the second step after a cache-served row before constructing storage
  scopes.
- Try one-row result-cache replay before constructing storage scopes for
  not-yet-executed statements; misses continue to the existing scoped MariaDB
  execution path.
- Keep cache misses, normal result fetching, writes, and MariaDB execution under
  the existing storage-context scope.

## Compatibility Impact

No SQL-visible behavior change is intended. The fast path only applies to
already-proven cached rows or already-done statements. Cache misses still enter
MariaDB through the existing path.

## Single-File And Lifecycle Impact

No durable `.mylite` format, companion-file, locking, recovery, or storage
lifecycle change.

## Public API And File-Format Impact

No public API or file-format change. Public `mylite_step()` results remain the
same.

## Storage-Engine Routing Impact

No routing policy change.

## Binary-Size And Dependency Impact

No dependency change. Binary-size impact is limited to a small helper and
branching in `mylite_step()`.

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
  `0.700 us/op`, done component `0.058 us/op`, reset component
  `0.021 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-pk-selects 10000 1000000`: prepared point selects
  `0.744 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-pk-select-reset-after-row 10000 1000000`: reset-after-row
  point selects `0.739 us/op`.

## Acceptance Criteria

- Existing prepared statement and storage-engine tests pass.
- Cached point-read row and done components avoid storage-context setup.
- Prepared point-select benchmarks improve or remain neutral.

## Risks And Open Questions

- Cache misses now perform one cheap cache probe before entering the scoped
  execution path. If a workload is almost entirely cold misses, this is a small
  extra branch/probe before the existing MariaDB path.

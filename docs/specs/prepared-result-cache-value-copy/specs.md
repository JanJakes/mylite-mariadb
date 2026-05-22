# Prepared Result Cache Value Copy

## Problem

After the prepared one-row result cache became set-associative, the 10k-row
prepared primary-key benchmark spent most of the cache-hit path copying
`std::vector<ColumnValue>` instances. The hot result shape is one fixed-width
integer column, but cache replay still used whole-vector assignment, which
copies nested `std::vector<unsigned char>` members and runs generic container
bookkeeping for every cached row.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc::execute_cached_one_row_result()` copies
  cache entries back into the active statement before returning `MYLITE_ROW`.
- `packages/libmylite/src/database.cc::capture_one_row_result_cache_row()` and
  `put_one_row_result_cache_entry()` publish rows only after MariaDB has
  produced the row and the statement has proven one-row exhaustion.
- A local sample of `prepared-pk-select-components` after cache associativity
  showed `std::vector<ColumnValue>::operator=()` and `ColumnValue::operator=()`
  dominating cache-hit replay.

## Design

- Add MyLite-owned cached-column copy helpers.
- Reuse already-sized `ColumnValue` vectors and copy scalar fields in place.
- Copy byte buffers only when the source column type is `TEXT` or `BLOB`.
- Use the same helper for cache replay, pending cache-row capture, and cache
  entry publication.
- Preserve the pending row buffer across normal execution-state resets so miss
  paths can reuse the already-sized column vector; full cache invalidation still
  releases it.
- Preserve the existing bad-allocation handling: allocation failures clear the
  cache and surface `MYLITE_NOMEM` where replay must report an error.

## Compatibility Impact

No SQL-visible behavior change is intended. Values are still produced by
MariaDB before caching and replayed under the same retained read scope. Variable
columns still copy their bytes, while fixed-width scalar columns avoid
irrelevant byte-vector work.

## Single-File And Lifecycle Impact

No durable `.mylite` format, companion-file, locking, recovery, or storage
lifecycle change.

## Public API And File-Format Impact

No public API or file-format change.

## Storage-Engine Routing Impact

No routing policy change.

## Binary-Size And Dependency Impact

No dependency change. Binary-size impact is limited to two local helper
functions in `libmylite`.

## Test And Verification Plan

- Run `git diff --check`.
- Run `git clang-format --diff -- packages/libmylite/src/database.cc`.
- Build `mylite_embedded_statement_test`,
  `mylite_embedded_storage_engine_test`, and `mylite_perf_baseline`.
- Run focused embedded statement and storage-engine CTest coverage.
- Run the prepared primary-key component benchmark with a 10k-row working set.

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
  `0.757 us/op`, done component `0.065 us/op`, reset component
  `0.029 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-pk-selects 10000 1000000`: prepared point selects
  `0.799 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-pk-select-reset-after-row 10000 1000000`: reset-after-row
  point selects `0.777 us/op`.

## Acceptance Criteria

- Existing prepared result-cache tests still pass, including write invalidation.
- Fixed-width cache replay no longer spends the hot path in whole-vector
  assignment.
- Normal cache execution cleanup no longer destroys the inactive pending row
  vector.
- Variable-width cache entries continue to copy their byte buffers.

## Risks And Open Questions

- This optimizes cache replay, not cold MariaDB prepared execution.
- The helper intentionally leaves non-variable byte vectors untouched because
  fixed-width getters ignore them and clearing them would reintroduce work on
  the scalar hot path.
- Retaining the inactive pending row vector keeps one row of memory until cache
  invalidation or statement finalization. That is bounded by the existing
  statement-owned cache lifecycle.

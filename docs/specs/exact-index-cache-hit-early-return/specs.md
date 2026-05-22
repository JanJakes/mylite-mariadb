# Exact Index Cache Hit Early Return

## Problem

Prepared primary-key update sampling still shows
`find_exact_index_row_id()` on the hot path after active exact-index caches have
been loaded. On a cache hit, the function already has the authoritative row id,
but it still falls through the rest of the miss pipeline's guard checks before
running empty cleanup for row-id lists and owned catalog images.

That extra control flow is small, but it sits inside every accepted direct
prepared point update.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- The change is first-party storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source change is
  required.
- `find_exact_index_row_id()` probes the active exact-index cache first with
  `find_existing_cached_exact_index_entry_in_statement()`.
- When that helper finds an exact-index cache, it sets `used_cache` whether the
  requested key is present or absent, because the cache is complete for that
  table/index/key-size view.
- The current function then relies on `!used_cache` checks to skip the durable
  cache, leaf-root, and append-history fallbacks, but still executes several
  branches and empty cleanup before returning.
- The same complete-cache semantics apply after
  `load_cached_exact_index_entry_in_statement()` and
  `find_cached_durable_exact_index_entry()`.

## Design

- Return immediately from `find_exact_index_row_id()` when a successful exact
  cache probe sets `used_cache`.
- Return immediately after a cache-probe error, preserving the current error
  result.
- After loading an owned catalog image, keep cleanup before returning from
  cache-hit paths that may have materialized the catalog.
- Leave leaf-root and append-history fallback behavior unchanged.

## Affected Subsystems

- MyLite storage exact-index row-id lookup.
- Prepared primary-key update and point-read execution that hits active or
  durable exact-index caches.

## Compatibility Impact

No SQL, public C API, storage-engine routing, metadata, transaction, or
file-format behavior changes. The early return only exits after the same cache
lookup has already determined the result that the old function returned.

## Single-File And Embedded Lifecycle Impact

No durable file, sidecar, lock, journal, recovery, or handle-lifecycle change.

## Public API And File-Format Impact

No public API, internal storage API, or `.mylite` file-format change.

## Binary-Size And Dependency Impact

Small first-party control-flow change. No dependency change.

## Tests And Verification

- `git diff --check`
- `git clang-format --diff -- packages/mylite-storage/src/storage.c`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-update-components 1000 1000000`
- Focused macOS sample of the same prepared-update component phase.

Completed verification:

- `git diff --check`: pass.
- `git clang-format --diff -- packages/mylite-storage/src/storage.c`: pass.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`: pass.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  pass, 2/2 tests.
- `ctest --preset storage-smoke-dev --output-on-failure`: pass, 10/10 tests.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-update-components 1000 1000000`: prepared primary-key
  update step measured 1.689 us/op.
- Focused macOS sample written to
  `/tmp/mylite-exact-cache-hit-early-return.sample.txt`; the sample remains
  noisy and still shows exact-index lookup frames, but active cache hits now
  avoid the fallback guard pipeline.

## Acceptance Criteria

- Active and durable exact-index cache hits return from
  `find_exact_index_row_id()` without walking the fallback guard pipeline.
- Cache miss and no-cache paths still reach leaf-root or append-history
  fallbacks as before.
- Existing storage and embedded storage-engine tests continue passing.
- Benchmark/profile notes record the resulting hot-path shape.

## Risks And Unresolved Questions

- The early return relies on complete exact-index cache semantics. If a future
  partial cache is introduced, it must not set `used_cache` unless absence is
  authoritative for that lookup.

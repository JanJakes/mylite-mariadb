# Exact Index Cache Set Last Hit

## Problem

Prepared primary-key updates now repeatedly hit the active exact-index cache
before row materialization. Sampling shows remaining storage-side time inside
`find_exact_index_row_id()`, including the inlined exact-index cache-set lookup
before the cached key bucket is probed.

Most hot row-DML loops keep using the same table, index number, and key size.
The cache descriptor lookup is therefore invariant while the statement-owned or
thread-local exact-index cache set remains unchanged.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite owns the exact-index cache implementation in
  `packages/mylite-storage/src/storage.c`.
- `find_exact_index_row_id()` first probes the active statement cache through
  `find_existing_cached_exact_index_entry_in_statement()`, then falls back to
  loading or seeding the cache, published leaf roots, durable caches, and
  append-history scans.
- `find_exact_index_cache()` scans the cache-set entries by table id, index
  number, and key size on every active cache probe.
- Durable exact-index caches have the same table/index/key lookup, but also
  require filename, catalog root, catalog generation, and page-count validation.
- Existing row-payload and live-row cache sets already use a validated
  last-hit index to avoid repeated tiny cache-set scans.

## Design

- Add a bounded last-hit index to `mylite_storage_exact_index_cache_set`.
- Probe the last-hit entry before the linear scan in `find_exact_index_cache()`,
  validating table id, index number, and key size before returning it.
- Update the hint when a lookup or append succeeds; clear it on miss.
- Add the same last-hit hint to `find_durable_exact_index_cache()`, but validate
  the durable-only filename and header identity fields before returning.
- Reset durable last-hit hints when durable exact-index caches are compacted or
  retargeted, and reset append-created hints if a later load/copy step rolls the
  append back.

## Affected Subsystems

- MyLite active and durable exact-index caches.
- Prepared primary-key update row lookup.
- Storage-smoke performance baseline.

## Compatibility Impact

No SQL, public API, storage-engine routing, metadata, or file-format behavior
change. The optimization returns only cache entries that pass the same identity
checks as the existing scan.

## Single-File And Lifecycle Impact

No durable storage, journal, lock, or companion-file lifecycle change. The
last-hit index is transient process memory and is cleared with the cache set.

## Public API And File-Format Impact

No public API, internal storage API, or file-format change.

## Binary-Size And Dependency Impact

Small first-party C state fields and branches. No dependency change.

## Tests And Verification

- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline` with the `storage-smoke-dev` preset.
- Run `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Run `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`.
- Run full `ctest --preset storage-smoke-dev --output-on-failure`.
- Run `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`.
- Sample a focused prepared-update benchmark and check whether the
  `find_exact_index_row_id()` storage frame moves away from exact-index
  cache-set scanning.
- Run `git diff --check` and `git clang-format --diff` on the touched C file.

Completed verification:

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline` passed.
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test` passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed, 2/2 tests.
- `ctest --preset storage-smoke-dev --output-on-failure` passed, 10/10 tests.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000` recorded prepared primary-key updates
  at 2.287 us/op.
- The focused sampled run recorded 2.302 us/op in
  `/tmp/mylite-exact-index-cache-set-last-hit.sample`; `find_exact_index_cache`
  and `exact_index_cache_matches` were no longer visible sample frames.
  Remaining visible storage work was row-payload copy/materialization and active
  row rewrite.
- `git diff --check` passed, and the clang-format diff check for
  `packages/mylite-storage/src/storage.c` reported no formatting changes.

## Acceptance Criteria

- Active exact-index cache-set lookup reuses a validated last-hit descriptor
  for repeated table/index/key-size probes.
- Durable exact-index cache lookup reuses a validated last-hit descriptor only
  when filename and header identity still match.
- Cache rollback, compaction, retarget, and clear paths cannot return stale
  cache descriptors.
- Existing storage and embedded storage-engine tests pass.
- Benchmark/profile evidence records the prepared-update impact.

## Risks And Open Questions

- This removes only the descriptor lookup. The key bucket hash, row-payload copy,
  and MariaDB quick execution work remain visible and need separate evidence
  before changing.

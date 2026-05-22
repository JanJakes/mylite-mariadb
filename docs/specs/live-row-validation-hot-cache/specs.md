# Live Row Validation Hot Cache

## Problem

Focused prepared primary-key update profiling still shows
`validate_direct_live_row_in_statement_cache()` in the MyLite storage stack.
The hot prepared point-update path usually has already resolved the active
row-payload cache and either finds the row payload entry directly or finds a
validated live-row id in the statement cache.

The remaining cost is call-shape overhead around a helper that also contains
the cold storage-read and hidden-row validation fallback. Those fallback checks
must remain unchanged for cache misses and standalone storage callers, but they
should not define the repeated prepared update cache-hit path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `ha_mylite::update_row()` in `mariadb/storage/mylite/ha_mylite.cc` calls the
  MyLite storage update path after MariaDB has positioned the row by key.
- `update_row_with_index_entries()` in `packages/mylite-storage/src/storage.c`
  resolves the active row-payload cache and active live-row cache before
  calling `validate_direct_live_row_in_statement_cache()`.
- `validate_direct_live_row_in_statement_cache()` first checks the active
  row-payload cache, then the active validated live-row cache. On miss it reads
  the row page, verifies the table id, scans row-state pages through
  `row_is_hidden_after()`, and marks the row validated.
- The active row-payload and validated live-row caches are statement-local and
  keyed by catalog root, catalog generation, table id, and row id.

## Design

- Mark `validate_direct_live_row_in_statement_cache()` as
  `MYLITE_STORAGE_HOT_INLINE`.
- Keep only addressability, optional payload-cache resolution, active
  row-payload cache hit, active live-row-cache resolution, and validated
  live-row-cache hit in the inline helper.
- Move row-page reads, table-id validation, hidden-row scanning, and cache-miss
  marking to a cold fallback helper.
- Preserve the existing returned row-page shape for cache hits and the existing
  storage-read behavior for misses.

## Affected Subsystems

- MyLite active row-payload cache lookup.
- MyLite active live-row validation cache lookup.
- Prepared primary-key update validation before active buffered-page rewrite.

## Compatibility Impact

No SQL, public API, handler API, storage-engine routing, metadata, or
file-format behavior change. Cache misses still perform the same row-page and
row-state validation before marking a row live and validated.

## Single-File And Lifecycle Impact

No durable storage, journal, lock, or companion-file lifecycle change. The slice
changes only transient helper call shape.

## Public API And File-Format Impact

No public API, internal storage API, or file-format change.

## Binary-Size And Dependency Impact

Small first-party code-layout change. The cold validation fallback remains a
single helper, limiting inline growth.

## Tests And Verification

- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline` with the `storage-smoke-dev` preset.
- Run `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Run `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`.
- Run full `ctest --preset storage-smoke-dev --output-on-failure`.
- Run `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`.
- Sample a focused prepared-update benchmark and check whether the direct live
  row validation wrapper remains a visible storage frame.
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
  at 2.412 us/op.
- The focused sampled run recorded 2.451 us/op in
  `/tmp/mylite-live-row-validation-hot-cache.sample`.
  `validate_direct_live_row_in_statement_cache()` and
  `load_direct_live_row_in_statement_cache()` were not visible sampled frames.
  Remaining visible storage work was active single-index rewrite,
  buffered-page undo capture, active live-row-id seeding, and handler-side key
  preparation.
- `git diff --check` passed, and `git clang-format --diff` for
  `packages/mylite-storage/src/storage.c` reported no formatting changes.

## Acceptance Criteria

- Repeated prepared point-update row validation returns from the inlined active
  row-payload or validated live-row cache-hit branch.
- Cache misses still read and validate the row page, reject hidden rows, and
  mark the live row as both live and validated.
- Existing storage and embedded storage-engine tests pass.
- Focused benchmark/profile evidence records the effect and remaining hot path.

## Risks And Open Questions

- This does not remove row validation from the update path; it only makes the
  already-cached path cheaper. Broader validation elision would need a separate
  proof that exact-index lookup and statement cache state fully imply row
  liveness for all relevant callers.

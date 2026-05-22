# Active Row Payload Replace Hot Entry

## Problem

Prepared primary-key update profiling still samples
`replace_active_row_payload_in_cache()` after row validation and active buffered
page rewrite. In the hot path, validation already returned the active
row-payload cache entry for the row, the rewrite keeps the row id unchanged, and
the encoded row payload size is stable across `value = value + 1` updates.

The helper still contains bucket lookup, row-id remap, removal, capacity, and
resize logic in the same frame. Those cases are needed for broader update
shapes, but the repeated known-entry/same-size replacement should update the
cached bytes directly.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `validate_direct_live_row_in_statement_cache()` can return the active
  row-payload cache entry through `out_active_payload_entry`.
- `update_row_with_index_entries()` passes that known entry into
  `replace_active_row_payload_in_cache()` after publishing the active rewrite.
- `replace_active_row_payload_in_cache()` already has a same-row replacement
  branch, but the function frame also includes cold bucket lookup, row-id remap,
  cache removal, and resize handling.

## Design

- Mark `replace_active_row_payload_in_cache()` as `MYLITE_STORAGE_HOT_INLINE`.
- Add an inline known-entry fast path for same-row, same-size replacements.
- In that fast path, copy the new row bytes into the known cached entry and
  clear its checksum so active-cache consumers continue to trust active
  statement ownership while durable cache consumers still validate checksums.
- Move bucket lookup, capacity checks, same-row resize, removal, and row-id
  remap behavior into a cold fallback helper.

## Affected Subsystems

- MyLite active row-payload cache replacement.
- Prepared primary-key update cache maintenance after active buffered-page
  rewrite.

## Compatibility Impact

No SQL, public API, handler API, storage-engine routing, metadata, or
file-format behavior change. The cold fallback preserves existing behavior for
resizes, row-id changes, removal, and bucket lookup misses.

## Single-File And Lifecycle Impact

No durable storage, journal, lock, or companion-file lifecycle change. The slice
changes only transient active cache maintenance.

## Public API And File-Format Impact

No public API, internal storage API, or file-format change.

## Binary-Size And Dependency Impact

Small first-party inline/cold split. No dependency or build-profile change.

## Tests And Verification

- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline` with the `storage-smoke-dev` preset.
- Run `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Run `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`.
- Run full `ctest --preset storage-smoke-dev --output-on-failure`.
- Run `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`.
- Sample a focused prepared-update benchmark and check whether
  `replace_active_row_payload_in_cache()` remains a visible storage frame.
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
  at 2.301 us/op.
- The focused sampled run recorded 2.306 us/op in
  `/tmp/mylite-active-row-payload-replace-hot-entry.sample`.
  `replace_active_row_payload_in_cache()` and
  `replace_active_row_payload_in_cache_slow()` were not visible sampled frames.
  Remaining visible storage work was active single-index rewrite,
  buffered-page undo capture, live-row-id durable promotion, exact-index cache
  replacement, and handler-side key preparation.
- `git diff --check` passed, and `git clang-format --diff` for
  `packages/mylite-storage/src/storage.c` reported no formatting changes.

## Acceptance Criteria

- Repeated prepared point-update active row-payload replacement returns through
  the inlined known-entry same-size path.
- Cold replacement behavior remains unchanged for row-id remaps, removals,
  bucket lookup, and payload-size changes.
- Existing storage and embedded storage-engine tests pass.
- Focused benchmark/profile evidence records the effect and remaining hot path.

## Risks And Open Questions

- The fast path intentionally handles only same-size replacements. Broader
  payload-size changes still use the existing fallback to preserve cache
  accounting and allocation failure behavior.

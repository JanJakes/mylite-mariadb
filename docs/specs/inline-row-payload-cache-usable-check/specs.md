# Inline Row-Payload Cache Usable Check

## Problem

After active row-payload bucket hints and deferred live-row cache lookup, the
sampled `prepared-row-only-update-components` profile still shows
`row_payload_cache_entry_is_usable()` as a named child under
`read_indexed_row_payload_from_open_file()`. The helper only checks whether a
cache entry and its owned row pointer are present, so each cached indexed-row
read pays an avoidable function call around a two-condition predicate.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This slice is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source change is
  required.
- `read_indexed_row_payload_from_open_file()` checks active and durable
  row-payload cache entries with `row_payload_cache_entry_is_usable()` before
  copying cached row bytes.
- `append_cached_row_payload_to_builder()` uses the same predicate for batched
  row-id materialization.
- The helper has no side effects and can be hot-inlined without changing cache
  invalidation, row visibility, or fallback behavior.

## Design

Mark `row_payload_cache_entry_is_usable()` as `MYLITE_STORAGE_HOT_INLINE`.
Leave its predicate unchanged.

## Compatibility Impact

No SQL-visible, public C API, storage-engine routing, metadata, or file-format
behavior change. The same entries are accepted or rejected.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle change.

## Public API And File-Format Impact

No public API or `.mylite` format change.

## Binary-Size And Dependency Impact

No dependency change. Binary size may change trivially due to inlining one
small predicate at three first-party call sites.

## Tests And Verification Plan

Verified on 2026-05-23 on macOS 26.5 with:

- `git diff --check`
- `git clang-format --diff -- packages/mylite-storage/src/storage.c`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-row-only-update-components --profile-iterations=10000000 10000`

The 10,000,000-iteration unsampled run measured prepared row-only update step
component at `1.265 us/op`. The sampled run measured `1.284 us/op`; its sample
was written to `/tmp/mylite-inline-row-payload-cache-usable-check.sample.txt`
and no longer contained a `row_payload_cache_entry_is_usable()` frame.

## Acceptance Criteria

- Cached indexed-row and rowset materialization paths use the same usability
  predicate without a named helper frame.
- Existing storage and embedded storage-engine tests pass.
- Prepared row-only update timing does not regress.

## Risks And Unresolved Questions

- This is a micro-optimization. It should only be kept if tests pass and local
  timing remains stable.

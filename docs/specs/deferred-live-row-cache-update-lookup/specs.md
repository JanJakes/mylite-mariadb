# Deferred Live-Row Cache Update Lookup

## Problem

The prepared row-only update hot path reads the target row through an active
row-payload cache and then updates the same row id in the active append buffer.
`update_row_with_index_entries_for_context()` still resolves the active
live-row cache before validation, even though active row-payload cache hits do
not need that cache and same-row preserving-index rewrites do not need
post-update live-row retargeting.

The sampled `prepared-row-only-update-components` profile after the row-payload
bucket hint still shows `live_row_cache_for_statement()` and
`find_live_row_cache()` in the storage mutation path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This slice is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source change is
  required.
- `validate_direct_live_row_in_statement_cache()` accepts a nullable
  `mylite_storage_live_row_cache **` and resolves the live-row cache only after
  active row-payload cache validation misses.
- `update_row_with_index_entries_for_context()` currently resolves the
  live-row cache before calling that helper.
- `replace_active_live_row_in_cache()` only needs an existing cache lookup when
  the storage update replaces the row id. Preserving-index active row-only
  rewrites keep `position.row_page_id == row_id`.
- Existing standalone live-row replacement helpers already resolve the cache
  locally for callers that do not have a row-update-local cache pointer.

## Design

- Stop resolving `active_live_row_cache` before row validation.
- Continue passing the nullable cache pointer to
  `validate_direct_live_row_in_statement_cache()`, preserving its fallback
  behavior for payload-cache misses.
- Before post-update live-row retargeting, resolve the live-row cache only when
  the update actually changed the row id and validation did not already resolve
  the cache.
- Keep active row-payload cache resolution unchanged.

## Compatibility Impact

No SQL-visible, public C API, storage-engine routing, metadata, or file-format
behavior change. Live-row cache updates still happen after a successful storage
mutation, and fallback validation still marks rows through the same cache when
it needs one.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle change.
The slice only changes transient active statement cache lookup timing.

## Public API And File-Format Impact

No public API or `.mylite` format change.

## Binary-Size And Dependency Impact

No dependency change. Binary-size impact is limited to a few branches removed
or moved in first-party storage code.

## Tests And Verification Plan

- Passed `git diff --check`.
- Passed `git clang-format --diff -- packages/mylite-storage/src/storage.c`.
- Passed `cmake --build --preset storage-smoke-dev --target
  mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`.
- Passed `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Passed `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`.
- Passed `ctest --preset storage-smoke-dev --output-on-failure`.
- Ran `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components --profile-iterations=10000000
  10000`:
  - bind: `0.022 us/op`
  - step: `1.268 us/op`
  - reset: `0.022 us/op`
- Ran a sampled 10M-iteration `prepared-row-only-update-components` benchmark:
  - bind: `0.022 us/op`
  - step: `1.285 us/op`
  - reset: `0.023 us/op`
  - sample written to
    `/tmp/mylite-deferred-live-row-cache-update-lookup.sample.txt`.
  - `live_row_cache_for_statement()` and `find_live_row_cache()` no longer
    appear in the sampled row-only update path.

## Acceptance Criteria

- Active row-payload-cache-hit update validation no longer requires an eager
  live-row cache lookup.
- Row-id-changing updates still resolve and update the live-row cache before
  retargeting cached live-row state.
- Existing storage and embedded storage-engine tests pass.
- Prepared row-only update timing does not regress.

## Risks And Unresolved Questions

- The deferred lookup must not leave old row ids in an existing live-row cache
  when a storage update appends a replacement row. The implementation resolves
  the cache before retargeting when `position.row_page_id != row_id`.
- This does not remove live-row cache work from validation fallback paths where
  no active row-payload cache entry exists.

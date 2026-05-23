# Deferred Cache Retarget Header Fingerprint

## Problem

Prepared row-only updates now avoid append-replacement cache work when active
rewrites keep the row id unchanged, but the storage update path still records a
deferred durable-cache retarget marker for every successful mutation. The marker
stores a full `mylite_storage_header` even though durable row-id, row-payload,
exact-index, and leaf-page cache retargeting only use the durable cache identity
fingerprint: catalog root page, catalog generation, and page count.

A sample after the row-only retarget matcher removal still showed
`defer_durable_cache_retarget_after_table_mutation()` and
`merge_deferred_durable_cache_retarget()` in the prepared row-only update hot
path. Copying a full header there is unnecessary.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::direct_update_rows()`
  routes accepted exact-key prepared updates through MyLite storage inside the
  active storage checkpoint.
- `packages/mylite-storage/src/storage.c::retarget_durable_caches_after_table_mutation()`
  delegates to durable live-row-id, row-payload, index-leaf-page, and
  exact-index cache retargeting.
- Those retargeting functions compare or rewrite only `catalog_root_page`,
  `catalog_generation`, and `page_count`; index-leaf-page table mutation
  retargeting clears the file-local leaf cache and ignores the header.
- Deferred retarget markers are applied before top-level active caches are
  promoted, so a compact fingerprint is sufficient to reconstruct the minimal
  header view needed by durable cache retargeting.

## Design

- Replace the deferred durable/catalog cache retarget header fields stored on
  `mylite_storage_statement` with a compact header-fingerprint struct.
- Store only catalog root page, catalog generation, and page count when a
  mutation or catalog extension records a deferred retarget marker.
- Materialize a minimal `mylite_storage_header` at apply time for the existing
  durable cache retarget helpers, keeping the helper signatures and cache
  invalidation logic unchanged.
- Keep table-id and all-tables marker behavior unchanged.

## Compatibility Impact

No SQL-visible behavior change is intended. Cache invalidation and retargeting
continue to use the same durable cache identity fields as before.

## Single-File And Embedded Lifecycle Impact

No file-format, journal, lock, sidecar, or open/close lifecycle change.

## Public API And Storage Routing Impact

No public `libmylite` API or storage-engine routing change.

## Binary-Size And Dependency Impact

No dependency change. The statement struct keeps less deferred-retarget state.

## Tests And Verification

- Passed `git diff --check`.
- Passed `git clang-format --diff -- packages/mylite-storage/src/storage.c`.
- Passed `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline -j1`
- Passed `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Passed `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`.
- Passed `ctest --preset storage-smoke-dev --output-on-failure`.
- Ran `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components --profile-iterations=5000000
  10000`:
  - bind: `0.022 us/op`
  - step: `1.560 us/op`
  - reset: `0.022 us/op`
  - checksum: `5000000`

## Acceptance Criteria

- Deferred cache retarget markers retain the same table/all-table semantics.
- Durable cache retargeting still uses catalog root page, catalog generation,
  and page count.
- Existing storage and routed embedded tests pass.
- The prepared row-only update component benchmark remains stable or improves.

## Risks And Unresolved Questions

- If a future durable cache retarget helper depends on additional header
  fields, the compact fingerprint must be extended in the same change.

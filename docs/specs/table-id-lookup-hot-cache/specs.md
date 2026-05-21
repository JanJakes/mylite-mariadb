# Table ID Lookup Hot Cache

## Problem

Focused prepared primary-key update profiling still shows
`find_table_id_in_statement()` as a visible MyLite storage frame during
`update_row_with_index_entries()`. The function already checks the active
table-entry cache first, and hot prepared row-DML loops repeatedly use the same
schema/table under the same active statement cache.

The remaining cost is not catalog lookup in the common case; it is entering a
helper whose hot branch only returns the cached table id. Catalog materialization
should remain available as the fallback for the first lookup or invalidated
metadata, but it should not shape the repeated prepared update path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `ha_mylite::update_row()` passes schema/table names into MyLite storage update
  APIs from `mariadb/storage/mylite/ha_mylite.cc`.
- `update_row_with_index_entries()` calls `find_table_id_in_statement()` in
  `packages/mylite-storage/src/storage.c` before row validation and rewrite.
- `find_table_id_in_statement()` first probes
  `find_active_table_entry_cache_in_statement()`. On hit it returns the cached
  `table_id`; on miss it reads the catalog image, finds the table record, and
  stores the active table-entry cache.
- The active table-entry cache already validates catalog root, catalog
  generation, schema/table names, and table id identity.

## Design

- Mark `find_table_id_in_statement()` as `MYLITE_STORAGE_HOT_INLINE`.
- Keep only the active table-entry cache hit in the inline function.
- Move catalog image materialization and cache population to a non-inline
  fallback helper used only on cache miss.
- Preserve existing cache validation, catalog fallback behavior, and error
  results.

## Affected Subsystems

- MyLite active table-entry cache lookup.
- MyLite row update, row read, and validation paths that resolve table ids.
- Prepared primary-key update performance baseline.

## Compatibility Impact

No SQL, public API, handler API, storage-engine routing, metadata, or
file-format behavior change. Cache misses still read the same catalog image and
populate the same active table-entry cache.

## Single-File And Lifecycle Impact

No durable storage, journal, lock, or companion-file lifecycle change. The slice
changes only transient helper call shape.

## Public API And File-Format Impact

No public API, internal storage API, or file-format change.

## Binary-Size And Dependency Impact

Small first-party code-layout change. The cold catalog fallback remains a
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
- Sample a focused prepared-update benchmark and check whether
  `find_table_id_in_statement()` remains a visible storage frame.
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
  at 2.277 us/op.
- The focused sampled run recorded 2.320 us/op in
  `/tmp/mylite-table-id-lookup-hot-cache.sample`.
  `find_table_id_in_statement()` and `load_table_id_in_statement()` were not
  visible sampled frames. Remaining visible storage work was active
  single-index rewrite, undo-entry copy/initialization, live-row validation,
  and handler-side key preparation.
- `git diff --check` passed, and the clang-format diff check for
  `packages/mylite-storage/src/storage.c` reported no formatting changes.

## Acceptance Criteria

- Repeated table-id lookups return from the inlined active table-entry cache
  branch.
- Catalog fallback remains unchanged on cache miss.
- Existing storage and embedded storage-engine tests pass.
- Focused benchmark/profile evidence records the effect and remaining hot path.

## Risks And Open Questions

- This does not remove table-id lookup entirely. A future handler/storage API
  change could pass a resolved table id through more row-DML calls, but that is
  broader and needs separate compatibility review.

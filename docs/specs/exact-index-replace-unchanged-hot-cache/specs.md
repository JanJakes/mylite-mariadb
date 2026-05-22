# Exact Index Replace Unchanged Hot Cache

## Problem

Prepared primary-key update profiling still shows
`replace_active_exact_index_cache_entries_in_statement()` after the active row
rewrite. In the common benchmark path, the active exact-index cache contains the
primary-key lookup entry, the row id stays unchanged after active buffered-page
rewrite, and the changed index is the secondary `value` key, not the primary
exact cache.

The current helper handles every cache replacement shape in one frame. The hot
unchanged single-cache case should return before entering the broader remove,
replace-key, and append logic.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `update_row_with_index_entries()` calls
  `replace_active_exact_index_cache_entries_in_statement()` after a successful
  update when index entries are not preserved wholesale.
- The handler passes `index_entry_changed` for all index entries. The prepared
  primary-key update keeps the primary exact key unchanged and changes the
  secondary `value` key.
- Existing replacement logic already treats `old_row_id == new_row_id`,
  matching cache entry, and unchanged key as a no-op.

## Design

- Mark `replace_active_exact_index_cache_entries_in_statement()` as
  `MYLITE_STORAGE_HOT_INLINE`.
- Add an inline fast return for one active exact-index cache where the cache
  matches an index entry marked unchanged and the row id is unchanged.
- Move the existing multi-cache, changed-key, row-id remap, removal, and append
  behavior into a cold fallback helper.
- Preserve existing behavior for missing matches by using the fallback rather
  than assuming the caller provided a complete index-entry set.

## Affected Subsystems

- MyLite active exact-index cache maintenance.
- Prepared primary-key update cache maintenance after active buffered-page
  rewrite.

## Compatibility Impact

No SQL, public API, handler API, storage-engine routing, metadata, or
file-format behavior change. Changed-key replacement, row-id remap, cache
removal, and multi-cache behavior remain in the fallback.

## Single-File And Lifecycle Impact

No durable storage, journal, lock, or companion-file lifecycle change. The slice
changes only transient active exact-index cache maintenance.

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
  `replace_active_exact_index_cache_entries_in_statement()` remains a visible
  storage frame.
- Run `git diff --check` and `git clang-format --diff` on the touched C file.

Completed verification:

- Built `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline` with the `storage-smoke-dev` preset before the final
  test run.
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed: 2 tests, 34.76 seconds.
- `ctest --preset storage-smoke-dev --output-on-failure` passed: 10 tests,
  40.54 seconds.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000` measured prepared primary-key updates
  at 2.283 us/op.
- A sampled prepared-update run measured 2.300 us/op. The sample did not show
  `replace_active_exact_index_cache_entries_in_statement()` or its slow helper
  as visible frames. Remaining storage samples were concentrated in
  `update_row_with_index_entries()` and `rewrite_active_update_pages()`, with
  smaller handler-side samples in
  `mylite_prepare_checked_index_entries_with_scratch()` and `key_copy()`.

## Acceptance Criteria

- Repeated prepared point-update exact-index cache maintenance returns from the
  inlined unchanged single-cache path when the cached key did not change.
- Changed keys, row-id remaps, removals, missing matches, and multi-cache cases
  continue through the existing fallback behavior.
- Existing storage and embedded storage-engine tests pass.
- Focused benchmark/profile evidence records the effect and remaining hot path.

## Risks And Open Questions

- This fast path deliberately handles only a single active exact-index cache.
  Broader changed-key and multi-cache cases need the existing full replacement
  logic.

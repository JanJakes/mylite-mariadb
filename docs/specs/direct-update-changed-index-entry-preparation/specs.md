# Direct Update Changed Index Entry Preparation

## Problem

Accepted exact-key MyLite direct updates already know which indexes may change
from the handler's direct-update key-change mask. `ha_mylite::update_row()`
still prepares new key images for every index before `mylite_prepare_index_entry_changes()`
marks stable keys unchanged. In the prepared-update component sample, that keeps
`key_copy()` visible even for stable keys on the direct-update path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::direct_update_rows_init()`
  fills `direct_update_key_may_change[]` after the accepted direct-update
  gates.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::update_row()` passes that
  mask to `mylite_update_preserves_all_index_entries()` and
  `mylite_prepare_index_entry_changes()`, but
  `mylite_prepare_checked_index_entries_with_scratch()` still builds a full
  entry list first.
- `packages/mylite-storage/src/storage.c::update_row_with_index_entries_for_context()`
  already treats index entries by their explicit `index_number`, and helper
  paths skip unchanged entries via the optional `index_entry_changed` bitmap.
- `packages/mylite-storage/src/storage.c::replace_active_exact_index_cache_entries_in_statement_slow()`
  must treat a missing entry in a sparse direct-update list as unchanged when
  the row id is stable; if the row id changes and the new key for a cached
  index is absent, clearing the exact-index caches is safer than leaving a
  false negative in a complete cache.
- Handler duplicate-key checks currently assume a full entry array indexed by
  table key number; they must be made entry-number aware before passing a
  sparse direct-update entry list.

## Design

- Extend the private handler index-entry preparation helper with an optional
  per-key "may change" filter.
- Keep existing insert, FK-action, and non-direct update callers unfiltered.
- For accepted direct updates, prepare entries only for keys whose key-change
  mask is set.
- Update index-entry change detection to validate each entry by its
  `index_number` rather than assuming `entry_index == key_number`.
- Update duplicate-key checks to iterate prepared entries and use each entry's
  `index_number`.
- Keep exact-index caches correct for sparse direct-update lists: omitted
  entries are unchanged when the row id is stable, and caches are cleared when
  a row-id change cannot be retargeted from the sparse list.

The storage C API is not changed. Sparse entry lists remain private to the
handler and are still explicit about the original table index number.

## Affected Subsystems

- MyLite storage handler direct-update row writes.
- MyLite exact-index cache maintenance after storage row updates.
- Handler duplicate-key checks for MyLite durable and volatile rows.
- Prepared-update performance baseline.

No SQL-layer prepare, engine routing, catalog, or public API behavior changes.

## Compatibility Impact

No SQL-visible behavior change is intended. The filter is conservative: when no
filter is supplied, all current paths prepare the same full index-entry list as
before. On the direct-update path, the filter is derived from MariaDB's update
write set after direct-update admission. Keys with uncertain metadata remain
marked as may-change by the existing key-change mask.

## Single-File And Embedded Lifecycle Impact

No file-format, sidecar, lock, recovery, or embedded lifecycle change. Durable
index maintenance still runs through the existing storage update APIs.

## Public API And File-Format Impact

No public `libmylite`, MyLite storage C API, or `.mylite` file-format change.

## Binary-Size And Dependency Impact

Small private handler changes only; no new dependency.

## Tests And Verification

- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  passes; `libmariadbd.a` remains 20.21 MiB.
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
  passes.
- `ctest --test-dir build/storage-smoke-dev -R 'libmylite.embedded-storage-engine' --output-on-failure`
  passes.
- `ctest --preset storage-smoke-dev --output-on-failure` passes.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-update-components 1000 10000`
  completes with:
  - bind: 0.120 us/op;
  - execute: 4387.370 us/op;
  - reset: 0.131 us/op.
- A one-second profiler sample during
  `mylite_perf_baseline --phase=prepared-update-components 1000 1000000`
  no longer shows `key_copy()` or
  `mylite_prepare_checked_index_entries_with_scratch()` as visible hot-path
  frames for the accepted direct-update loop. The sample instead shows
  exact-index row-id scanning under `find_exact_index_row_id()` as the dominant
  cost. The 1,000,000-iteration run was stopped after more than five minutes
  because it was measuring that separate scan-scaling bottleneck rather than
  this slice's key-image preparation cost.

## Acceptance Criteria

- Prepared exact-key updates keep existing affected-row, duplicate-key,
  secondary-index, FK, generated-column, CHECK, and rollback behavior.
- Insert and non-direct update paths still prepare full index-entry lists.
- Accepted direct updates prepare key images only for entries that may change.
- Sparse direct-update entry lists do not corrupt duplicate checks, maintained
  index root updates, exact-index caches, or active row rewrite behavior.

## Risks And Unresolved Questions

- A stale or overly narrow key-change mask would skip index maintenance. This
  slice relies on the existing conservative `direct_update_key_may_change[]`
  computation and does not change that computation.
- Sparse entry lists are private to direct updates for now. Broader use should
  be a separate slice with broader tests.

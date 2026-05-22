# Direct Update Preserve Probe Elision

## Problem

Accepted MyLite direct updates cache a per-index `direct_update_key_may_change`
mask during `direct_update_rows_init()`, but `ha_mylite::update_row()` still
calls `mylite_update_preserves_all_index_entries()` before preparing index
entries. For prepared point updates that write a secondary indexed column, that
precheck repeats old/new key-part byte comparisons before
`mylite_prepare_index_entry_changes()` performs the authoritative changed-entry
comparison for the same update.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::direct_update_rows_init()` runs after
  MariaDB has fixed the update field list and table write set.
- The existing `direct_update_key_may_change[MAX_KEY]` mask marks indexes whose
  key parts may be written by the accepted direct-update statement.
- `ha_mylite::update_row()` passes that mask into
  `mylite_prepare_index_entry_changes()`, which still compares old and new key
  bytes for every index marked changeable.
- `mylite_storage_update_row_with_index_entry_changes()` accepts a changed-entry
  bitmap. If the bitmap has no changed entries, storage uses the row-only update
  shape and retargets active exact-index caches through the supplied entries.

## Design

- Add a handler-local boolean that records whether any index may change for the
  accepted direct-update statement.
- Reset the boolean anywhere direct-update state is cleared or replaced.
- Fill it together with `direct_update_key_may_change[]` in
  `direct_update_rows_init()`.
- While `direct_update_rows()` is inside `update_row()`:
  - preserve all index entries immediately when the cached mask says no index
    can change,
  - otherwise skip the separate preserve-all probe and let
    `mylite_prepare_index_entry_changes()` produce the data-driven changed
    bitmap.

## Compatibility Impact

No SQL semantics change. MariaDB still evaluates the `WHERE` condition,
assignments, constraints, and row comparison before MyLite updates storage. For
indexes that may change, the old/new key comparison still happens in the
changed-entry bitmap path; if a statement writes an indexed field but the key
bytes remain unchanged, storage receives a no-changed-entry bitmap and performs
the existing row-only durable update shape.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle change.

## Public API And File-Format Impact

No public API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing-policy change. The slice only affects accepted MyLite handler direct
updates after the table is already routed to MyLite storage.

## Binary-Size And Dependency Impact

One handler boolean and a few branches. No new dependency.

## Tests And Verification

- `git diff --check`
- `git clang-format --diff -- mariadb/storage/mylite/ha_mylite.cc
  mariadb/storage/mylite/ha_mylite.h
  packages/libmylite/tests/embedded_storage_engine_test.c`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke
  tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `archive=build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a`
  - `size_bytes=21182536`
  - `size_mib=20.20`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - 2/2 tests passed.
- `ctest --preset storage-smoke-dev --output-on-failure`
  - 10/10 tests passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-update-components 10000 1000000`
  - `prepared primary-key update bind component`: `0.023 us/op`
  - `prepared primary-key update step component`: `2.097 us/op`
  - `prepared primary-key update reset component`: `0.023 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=storage-indexed-row-update-components 10000 1000000`
  - `storage indexed row update nested statement begin component`:
    `0.027 us/op`
  - `storage indexed row update mutation component`: `0.358 us/op`
  - `storage indexed row update nested statement commit component`:
    `0.056 us/op`

## Acceptance Criteria

- Direct updates whose write set cannot affect any index preserve index entries
  without a per-row preserve probe.
- Direct updates whose write set may affect an index skip the redundant
  preserve probe but still compute the changed-entry bitmap from old/new key
  bytes.
- Prepared exact-key updates that write an indexed prefix column but keep the
  index key bytes stable preserve row and index visibility.
- Existing routed storage update, rollback, FK, generated-column, and index
  tests pass.

## Risks And Unresolved Questions

- The cached boolean is only valid for the accepted direct-update statement, so
  it must be reset with the rest of direct-update state and used only while
  `direct_update_row_in_progress` is true.

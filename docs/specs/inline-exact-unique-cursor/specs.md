# Inline Exact Unique Cursor

## Problem

The routed update benchmark executes primary-key point updates. MariaDB reaches
the MyLite handler through `index_read_map()` before `update_row()`, and the
handler builds an exact unique index cursor for each primary-key lookup.

After lazy buffered page checksums, sampled storage-side time moved out of page
checksum generation and into handler update and index-cursor setup. The exact
unique cursor path currently heap-allocates a copied key, one cursor entry, and
optional one-row materialization offset/size arrays for every point lookup.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::index_read_map()` calls
  `build_index_cursor()` for exact primary-key lookups.
- `build_index_cursor()` has a raw exact unique fast path that produces at most
  one cursor entry.
- For non-BLOB durable tables, the fast path can fetch the row payload directly
  through `mylite_storage_find_indexed_row()`, then stores one row offset and
  one row size in heap arrays.
- General multi-entry and non-exact cursor paths still need dynamic entry/key
  storage.

## Design

- Add inline cursor storage to `ha_mylite` for the single-entry exact unique
  path:
  - one key buffer sized to MariaDB's `MAX_KEY_LENGTH`,
  - one `Mylite_index_cursor_entry`,
  - one row offset,
  - one row size.
- Use the inline storage when the exact unique key image fits the inline key
  buffer.
- Keep the existing heap path as a fallback for unexpectedly larger key images.
- Teach `clear_index_cursor()` not to free inline-owned pointers.
- Leave all general multi-entry cursor behavior unchanged.

## Affected Subsystems

- MyLite MariaDB storage-engine handler.
- Exact unique primary-key and unique-key point lookup cursors.
- Storage-smoke update performance baseline.

## Compatibility Impact

No SQL, public API, storage API, storage-engine routing, or MySQL/MariaDB
compatibility behavior changes. The cursor contains the same key, row id, and
optional materialized row payload as before; only ownership changes for the
single-entry exact unique case.

## Single-File And Lifecycle Impact

No durable file-format, companion-file, or storage lifecycle change.

## Public API And File-Format Impact

No public API or file-format change.

## Binary-Size And Dependency Impact

Small handler change. No new dependency. Handler instances gain a fixed inline
key buffer and a few scalar fields.

## Tests And Verification

- Rebuild the MariaDB storage-smoke embedded archive with
  `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build
  all -DPLUGIN_MYLITE_SE=STATIC`.
- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline` with the `storage-smoke-dev` preset.
- Run `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Run `ctest --test-dir build/storage-smoke-dev --output-on-failure`.
- Run `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=updates 1000
  1000000`.
- Run a sampled one-million-update benchmark with macOS `sample`.
- Run `git diff --check` and `git clang-format --diff` on touched C/C++ files.

Verification after implementation:

- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build
  all -DPLUGIN_MYLITE_SE=STATIC` rebuilt the storage-smoke embedded archive at
  `20.07 MiB`.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `ctest --test-dir build/storage-smoke-dev --output-on-failure` passed 10/10.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=updates 1000
  1000000`
- `git diff --check`
- `git clang-format --diff HEAD -- mariadb/storage/mylite/ha_mylite.cc
  mariadb/storage/mylite/ha_mylite.h`

Measured update baseline:

- Direct primary-key updates: `11.706 us/op`.
- Prepared primary-key updates: `5.092 us/op`.

Sampled rerun:

- Direct primary-key updates: `11.628 us/op`.
- Prepared primary-key updates: `4.972 us/op`.

The sampled cursor frame no longer showed key, entry, or one-row offset/size
allocation in the exact unique cursor setup. Remaining visible cursor cost is
mostly storage row materialization through `mylite_storage_find_indexed_row()`
and cleanup of the previously materialized row payload.

## Acceptance Criteria

- Exact unique cursors for small keys avoid per-lookup heap allocation.
- Large or unusual key images retain the existing heap fallback.
- Cursor cleanup never frees inline-owned storage.
- Existing storage and embedded storage-engine tests remain green.
- Update benchmark/profile evidence records the allocator work removed from
  exact unique cursor setup and the next remaining row-materialization cost.

## Risks And Open Questions

- This optimizes handler-side allocation overhead only. It does not change
  MariaDB parse, optimizer, lock, or expression-evaluation costs.
- Handler inline storage increases each handler object's footprint modestly.

# Dirty Entry Leaf Fill Cache

## Problem

Prepared-insert profiling still spends time in dirty-buffer merge and pressure
selection after maintained-root decode sites have been reduced to protected
planning and journal-validation reads. Test-hook slices already cache resident
dirty-buffer leaf occupancy facts for diagnostic recorders, but non-test
production pressure selection still reparses the same index-leaf header fields
from dirty-buffer entry page bytes whenever it needs entry count and capacity.

The remaining prepared-insert profile reports `31,938` merge pressure-context
builds, `19,053` planned stores, `21,031` dirty leaf pressure admissions, and
`66,144` merge direct writes. Each pressure-context build scans the resident
dirty-buffer entries and may call `dirty_page_buffer_entry_index_leaf_fill()`
for leaf candidates.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB SQL, handler, or
  storage-engine source is involved.
- `mylite_storage_dirty_page_buffer_entry` already caches page type in
  production builds and test-hook page family/leaf occupancy in test builds.
- `refresh_dirty_page_buffer_entry_page_type()` is called after every dirty
  entry admission or replacement path that changes page bytes.
- `dirty_page_buffer_pressure_complete_flush_context()` and
  `dirty_page_buffer_pressure_flush_index()` call
  `dirty_page_buffer_entry_index_leaf_fill()` while scanning the resident
  dirty-buffer entries.
- `dirty_page_buffer_entry_index_leaf_fill()` currently reparses key size,
  entry count, used bytes, and derived capacity directly from page bytes.

## Design

Cache production leaf fill facts on each dirty-buffer entry:

- add entry fields for cached leaf-fill presence, validity, entry count, and
  entry capacity;
- refresh those fields beside the existing page-type cache after every entry
  page mutation;
- make `dirty_page_buffer_entry_index_leaf_fill()` return cached facts when
  present and fall back to byte parsing for manually constructed or legacy test
  entries; and
- keep the existing test-hook occupancy fields and recorders unchanged.

This preserves pressure victim selection because it caches only facts that were
already read from the same resident page image.

## Compatibility Impact

No SQL-visible behavior, public C API behavior, handler API behavior,
storage-engine routing, metadata behavior, transaction semantics, recovery
semantics, or error-surface changes.

## Single-File And Lifecycle Impact

No file-format, journal, recovery, sidecar, lock, or embedded lifecycle change.
Dirty pages still publish through the same checksum refresh and journal
protection paths.

## Safety Boundary

This slice does not remove planning decodes, maintained-root validation,
checksum verification, or recovery-journal validation. It only caches resident
dirty-buffer leaf metadata already parsed from the mutable page image owned by
that entry.

## Test And Verification Plan

- Extend focused dirty leaf buffering coverage to assert cached entry count and
  capacity are populated after admission and still usable through
  `dirty_page_buffer_entry_index_leaf_fill()`.
- Reuse storage and storage-smoke tests for unchanged flush, rollback,
  recovery, and routed storage-engine behavior.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Dirty-buffer entry leaf fill facts refresh after admission and replacement.
- Pressure selection and merge direct-write structural counters stay unchanged.
- Prepared-insert maintained-root decode sites remain protected-only.

## Verification

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`:
  passed; no files modified.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `321.30 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline
  mylite_storage_test mylite_embedded_storage_engine_test`: passed; targets
  were already up to date.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `331.52 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke
  tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`: passed; archive
  size `33,993,994` bytes with `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-insert-components 1000 100000`: prepared insert step
  `71.769 us/op`; structural counters stayed unchanged at `8` full-page
  checksum calls, `127,063` zero-tail checksum calls, `5` protected
  maintained-root decodes, `21,031` dirty leaf pressure admissions, `66,144`
  merge direct writes, `87,176` index-leaf dirty refreshes, `31,938`
  pressure-context builds, and `19,053` planned stores.

## Risks

- Any entry mutation path that skips `refresh_dirty_page_buffer_entry_page_type()`
  would leave the fill cache stale. The implementation keeps refresh centralized
  in that existing post-mutation helper.

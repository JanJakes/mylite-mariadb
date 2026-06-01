# Dirty Buffer Entry Occupancy Facts

## Problem

Prepared-insert dirty-buffer merge and pressure profiling still records
`21,031` dirty leaf pressure admissions, `66,144` dirty leaf merge direct
writes, and `122,388` future-page relation rows. Earlier slices passed leaf
occupancy facts through some immediate recorders, but a resident dirty-buffer
entry can still have its index-leaf metadata parsed again by merge guard,
flush, rank, fallback-origin, and victim accounting paths.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite test-hook dirty-buffer bookkeeping in
  `packages/mylite-storage/src/storage.c`; upstream MariaDB does not own this
  storage layer.
- `store_dirty_page_in_buffer()` copies or mutates the resident entry page
  before later merge, pressure, and flush instrumentation inspects leaf
  occupancy.
- `dirty_page_buffer_index_leaf_occupancy()` derives the same leaf occupancy
  fields from page bytes for entry-owned callers: leaf shape, fill band,
  free-slot band, free-slot detail band, entry count, capacity, and free slots.

## Design

In test-hook builds, cache the derived leaf occupancy fact on each dirty-buffer
entry when the entry page is admitted or replaced. Add dirty-buffer entry
helpers that return the cached occupancy when present and fall back to parsing
page bytes for manually constructed test entries.

Use those helpers for entry-owned occupancy consumers in merge guard, flush
recording, flush rank/fill-band recording, pressure-victim accounting, fallback
origin slot derivation, and broad-victim direct-write accounting. Keep
standalone incoming page buffers on the direct parser path.

Do not change direct-write policy, pressure victim selection, checksum timing,
dirty-buffer replacement semantics, page bytes, journal protection, recovery
validation, maintained-root planning, or file format.

## Compatibility Impact

No SQL behavior, public C API behavior, storage-engine routing, metadata,
transaction, recovery, or compatibility support status changes. The cached
occupancy facts exist only in test-hook dirty-buffer entries.

## Single-File And Lifecycle Impact

No file lifecycle changes. Dirty-buffer entries still publish through the same
flush and direct-write paths, with the same checksum refresh and journal
protection requirements.

## Binary Size And Dependency Impact

No dependencies are added. Test-hook dirty-buffer entries gain cached leaf
occupancy fields. Non-test builds are unchanged except where prior page-type
helpers already apply.

## Tests And Verification Plan

- Extend focused self-test coverage to prove dirty-buffer entry occupancy facts
  are set at admission and updated after replacement.
- Existing dirty-buffer merge, pressure, flush, rollback, and recovery tests
  cover unchanged publication behavior.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Implementation Notes

- Added test-hook resident-entry leaf occupancy fields plus helpers that return
  cached facts for initialized dirty-buffer entries and retain the parser
  fallback for manually constructed test entries.
- Refreshed cached occupancy next to cached page-family facts after every
  dirty-buffer entry admission or replacement path.
- Reused cached occupancy for entry-owned merge guard, flush, rank,
  fallback-origin, pressure-victim, and broad-victim direct-write accounting.
  Standalone incoming pages still use direct byte parsing.
- Extended the focused dirty-buffer entry fact self-test to assert leaf
  admission and branch replacement update cached occupancy.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `315.13 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `335.01 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; archive size `33,989,418` bytes, `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed; full output saved at
  `/tmp/mylite-dirty-buffer-entry-occupancy-facts-benchmark.txt`.

The prepared-insert benchmark kept the structural counters unchanged:
`8` full-page checksum calls, `227,063` zero-tail checksum calls, `94,033`
dirty refreshes, `21,031` dirty leaf pressure admissions, `66,144` dirty leaf
merge direct writes, `87,176` index-leaf dirty refreshes, `677`
maintained-root decodes, `31,938` pressure-context builds, `19,053` planned
stores, `122,388` future-page relation rows, `125,212` non-leaf guard rows,
and `121` rejected below-tail candidate admissions. The sampled prepared
insert step was `76.886 us/op` under variable host load.

## Acceptance Criteria

- Dirty-buffer entry leaf occupancy facts are set for newly admitted entries
  and update when an existing entry is replaced by another page shape.
- Prepared-insert structural counters stay unchanged: merge direct writes,
  pressure admissions, dirty-refresh counts, checksum call counts,
  maintained-root decodes, pressure-context builds, planned stores,
  future-page relation rows, and non-leaf guard rows.
- Storage and embedded storage-engine smoke tests pass.

## Risks

- Cached occupancy facts must refresh after every entry page mutation. Keep
  parser fallback for entries without an initialized fact.
- The helper must only replace occupancy reads for resident dirty-buffer
  entries; standalone incoming pages still need direct byte parsing.

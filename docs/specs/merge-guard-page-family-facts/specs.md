# Merge Guard Page Family Facts

## Problem

The prepared-insert profile still records `122,388` future-page merge relation
rows and `122,388` future-current guard rows. The merge guard outcome recorder
classifies the child dirty-buffer page family, and the future-page relation
recorder classifies the same page family again for future pages. The previous
slice carried occupancy facts; page-family classification remains a redundant
test-hook parse in the same path.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test-hook accounting in
  `packages/mylite-storage/src/storage.c`.
- `test_dirty_page_buffer_flush_page_family()` reads the dirty-buffer page type
  and maps it to the checksum page family used by merge guard and relation
  counters.
- `record_dirty_page_buffer_merge_direct_write_guard_outcome()` calls that
  helper for every child dirty-buffer merge entry.
- `record_dirty_page_buffer_merge_future_page_relations()` calls the same
  helper for future-page rows after the guard has already seen the same entry.
- Current final benchmark evidence after the occupancy-facts slice reports
  `122,388` future-page relation rows, `66,144` dirty `index-leaf` merge
  direct writes, `21,031` pressure admissions, `87,176` index-leaf dirty
  refreshes, `8` full-page checksum calls, `227,063` zero-tail checksum calls,
  and `677` maintained-root decodes. These must remain unchanged.

## Design

Extend the test-hook-only merge guard facts object to carry the child entry's
page family when profiling is enabled. Pass that optional family into:

- `record_dirty_page_buffer_merge_direct_write_guard_outcome()`; and
- `record_dirty_page_buffer_merge_future_page_relations()`.

Both recorders keep their existing fallback page-family classification for
direct tests and future callers that do not supply guard facts.

Do not change the production guard signature, direct-write policy, pressure
selection, dirty-buffer admission, fallback-origin tagging, journaling, or
validation paths.

## Compatibility Impact

No SQL behavior, public C API behavior, storage-engine routing, file format,
durable bytes, or compatibility support status changes. This is
`MYLITE_STORAGE_TEST_HOOKS` profiling-source work only.

## Single-File And Lifecycle Impact

No file lifecycle changes. Dirty-page publication, checksum refresh, pressure
selection, direct-write decisions, journaling, rollback, and statement commit
behavior stay unchanged.

## Binary Size And Dependency Impact

No dependencies are added. Non-test-hook builds keep the existing guard
signature and should not gain page-family fields in the merge guard API.

## Tests And Verification

- Existing storage self-tests cover merge direct-write guard outcome counters,
  future-page relation counters, and planned pressure fallback replay.
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

Final verification passed:

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `build/dev/packages/mylite-storage/mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  (`421.50 sec`)
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  (`439.89 sec`)
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  (archive size `33,989,146` bytes, `478` members)
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  (`71.818 us/op`)

The final prepared-insert profile preserved the expected structural counters:
`122,388` future-page relation rows, `66,144` dirty `index-leaf` merge direct
writes, `21,031` dirty `index-leaf` pressure admissions, `87,176` index-leaf
dirty refreshes, `31,938` merge pressure-context builds, `19,053` planned
stores, `8` full-page checksum calls, `227,063` zero-tail checksum calls, and
`677` maintained-root decodes.

## Acceptance Criteria

- Merge direct-write guard outcome, future-relation, fill-band, free-slot, and
  free-slot-detail counters are unchanged.
- Prepared-insert checksum, maintained-root decode, pressure-admission, merge
  direct-write, and dirty-refresh counters are unchanged.
- Storage and embedded storage-engine tests pass.

## Risks

- Page-family facts must describe only the current child dirty-buffer entry.
  They must not be reused after the entry is copied, admitted, flushed, or
  mutated.

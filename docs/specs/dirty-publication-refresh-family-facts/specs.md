# Dirty Publication Refresh Family Facts

## Problem

The prepared-insert profile still records `87,178` dirty-page flush checksum
refreshes and the matching dirty-page publication checksum-source rows. In
test-hook builds, `refresh_dirty_buffered_page_checksum()` classifies the page
family for dirty checksum refresh counters, then
`record_dirty_page_publication_checksum_refresh()` classifies the same freshly
refreshed page family again before attributing the publication source.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test-hook accounting in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source path owns
  this custom dirty-page publication counter.
- `refresh_dirty_buffered_page_checksum()` already knows the checksum offset and
  records dirty checksum refreshes by family through
  `test_record_dirty_checksum_refresh()`.
- `write_dirty_page_buffer_entry()` then records publication-source attribution
  for the same refreshed dirty-buffer entry.
- The current prepared-insert profile reports `87,178` dirty-page flush
  refreshes: `87,176` `index-leaf` and `2` `index-branch`. Publication-source
  rows are `21,031` buffer-limit `index-leaf`, `66,144` merge-direct-write
  `index-leaf`, `1` statement-commit `index-leaf`, and `2` statement-commit
  `index-branch`.

## Design

Add a test-hook-only refresh helper that returns the page family already
computed for dirty checksum refresh accounting. `write_dirty_page_buffer_entry()`
uses that helper only when test hooks are enabled, then records publication
checksum-source counters from the carried family instead of parsing the page
again.

Keep the existing `refresh_dirty_buffered_page_checksum()` API as a wrapper for
all other callers and for non-test-hook builds. Publication-source recording
retains a family-validity guard, but it no longer needs the page bytes on the
dirty-buffer publication path.

Do not change dirty-page publication decisions, checksum bytes, pressure
selection, merge-direct-write policy, journal validation, or fallback dirty
checksum refresh behavior.

## Compatibility Impact

No SQL behavior, public C API behavior, storage-engine routing, file format,
durable bytes, or compatibility support status changes. This is
`MYLITE_STORAGE_TEST_HOOKS` profiling-source work only.

## Single-File And Lifecycle Impact

No file lifecycle changes. Dirty-page publication, recovery journals,
statement commit, rollback, and storage file bytes stay unchanged.

## Binary Size And Dependency Impact

No dependencies are added. Non-test-hook builds keep the existing dirty checksum
refresh API and should not gain publication-family helper state.

## Tests And Verification

- Existing storage self-tests cover dirty page publication checksum-source
  counters.
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
  (`313.75 sec`)
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  (`337.80 sec`)
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  (archive size `33,989,146` bytes, `478` members)
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  (`79.969 us/op` under variable host load)

The final prepared-insert profile preserved the expected structural counters:
`8` full-page checksum calls, `227,063` zero-tail checksum calls, `677`
maintained-root decodes, `87,178` dirty-page flush checksum refreshes,
`21,031` dirty leaf pressure admissions, `66,144` dirty leaf merge direct
writes, `87,176` index-leaf dirty refreshes, `31,938` merge pressure-context
builds, and `19,053` planned stores. Dirty page publication checksum-source
rows stayed `21,031` buffer-limit `index-leaf`, `66,144` merge-direct-write
`index-leaf`, `1` statement-commit `index-leaf`, and `2` statement-commit
`index-branch`. The checksum call-site table kept
`refresh_dirty_buffered_page_checksum` as the dirty-refresh checksum site.

## Acceptance Criteria

- Dirty checksum refresh source/family counters are unchanged.
- Dirty page publication checksum-source rows are unchanged.
- Prepared-insert checksum, maintained-root decode, dirty-refresh, pressure
  admission, and merge direct-write counters are unchanged.
- Storage and embedded storage-engine tests pass.

## Risks

- The carried family must only be used for the page just refreshed. If refresh
  fails or is bypassed, publication counters must not record stale family data.

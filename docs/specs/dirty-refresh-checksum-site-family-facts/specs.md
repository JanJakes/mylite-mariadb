# Dirty Refresh Checksum Site Family Facts

## Problem

After dirty-refresh counters reuse known page families, the checksum call-site
recorder still reparses the page header for every
`refresh_dirty_buffered_page_checksum` zero-tail checksum call. The current
prepared-insert profile reports these calls by family:

- row: `6,643`;
- index-entry: `212`;
- index-leaf: `87,176`; and
- index-branch: `2`.

Those `94,033` zero-tail checksum call-site rows are emitted from typed refresh
branches that have already matched the page family. Reclassifying the page for
call-site accounting is redundant profiling work, not checksum validation.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test-hook accounting in
  `packages/mylite-storage/src/storage.c`; upstream MariaDB does not own these
  checksum call-site counters.
- `refresh_dirty_buffered_page_checksum_with_family()` validates each page
  family before calling `refresh_dirty_buffered_page_checksum_zero_tail()`.
- `refresh_dirty_buffered_page_checksum_zero_tail()` calls
  `checksum_page_zero_tail_at_site()` with site name
  `refresh_dirty_buffered_page_checksum`.
- `checksum_page_zero_tail_at_site()` currently records both aggregate
  checksum family counters and call-site family counters by calling
  `test_checksum_page_family(page, checksum_offset)`.

## Design

Add test-hook checksum counter helpers that accept a known
`mylite_storage_test_checksum_page_family`.

Use a known-family zero-tail checksum wrapper only for
`refresh_dirty_buffered_page_checksum()` branches. Keep the generic
`checksum_page_zero_tail_at_site()` path unchanged for all other checksum
callers that do not already know the page family.

Do not change checksum bytes, checksum offsets, used-byte validation, dirty
checksum refresh selection, dirty-buffer publication, append-buffer flush,
maintained-root planning, journal validation, or non-test builds.

## Compatibility Impact

No SQL behavior, public C API behavior, storage-engine routing, file format,
metadata, transaction, or compatibility support status changes. This is
test-hook-only attribution work.

## Single-File And Lifecycle Impact

No file lifecycle changes. Dirty-page flushes, append-buffer flushes,
statement rollback, commit, recovery, and durable page validation stay on the
same paths.

## Binary Size And Dependency Impact

No dependencies are added. Non-test-hook builds keep the existing checksum
refresh code path.

## Tests And Verification Plan

- Extend checksum-family self-test coverage for known-family call-site
  recording so aggregate and site counters are updated without page parsing.
- Existing storage self-tests and storage-smoke coverage exercise dirty
  checksum refreshes, dirty-buffer flushes, append-buffer flushes, rollback,
  and recovery.
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

- `refresh_dirty_buffered_page_checksum` checksum call-site rows stay unchanged.
- Checksum family counters, dirty-refresh counters, maintained-root decode
  sites, dirty-page flush counts, append-buffer flush counts,
  pressure-admission counts, and merge direct-write counts stay unchanged.
- Storage and embedded storage-engine smoke tests pass.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `327.82 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `341.42 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; `libmariadbd.a` was `33,989,146` bytes with `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed with `74.220 us/op` prepared insert step under variable host load.

Current `prepared-insert-components 1000 100000` counters:

- `refresh_dirty_buffered_page_checksum` call-site rows: `6,643` row,
  `212` index-entry, `87,176` index-leaf, and `2` index-branch;
- full-page checksum calls: `8`;
- zero-tail checksum calls: `227,063`;
- maintained-root decodes: `677`;
- dirty-page-flush refreshes: `87,178`;
- append-buffer-flush refreshes: `6,849`;
- append-buffer-copy refreshes: `6`;
- total dirty checksum refreshes: `94,033`;
- buffer-limit pressure admissions: `21,031`;
- merge direct writes: `66,144`;
- index-leaf dirty refreshes: `87,176`;
- pressure-context builds: `31,938`;
- planned stores: `19,053`;
- future-page relation rows: `122,388`;
- rejected below-tail candidate admissions: `121`.

## Risks

- The known family must match the refresh branch. Restrict known-family
  call-site recording to the typed dirty-refresh branches and keep the generic
  parser path for all other checksum callers.

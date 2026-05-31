# Dirty Refresh Known Family Facts

## Problem

The prepared-insert profile still reports `87,178` dirty-page-flush checksum
refreshes and `94,033` total dirty checksum refreshes across dirty-page flush,
append-buffer flush, and append-buffer copy sources. In test-hook builds,
`refresh_dirty_buffered_page_checksum()` first identifies the page family by
entering the row, index-entry, index-leaf, maintained-root, or index-branch
refresh branch, then calls `test_record_dirty_checksum_refresh()`, which
reparses the same page header to classify the family for counters.

This duplicate classification is profiling overhead only. It does not validate
durable bytes and does not decide whether a checksum refresh is required.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test-hook accounting in
  `packages/mylite-storage/src/storage.c`; upstream MariaDB does not own these
  storage checksum counters.
- `refresh_dirty_buffered_page_checksum_with_family()` already selects a page
  family before refreshing:
  - row pages refresh `MYLITE_STORAGE_FORMAT_ROW_CHECKSUM_OFFSET`;
  - index-entry pages refresh `MYLITE_STORAGE_FORMAT_INDEX_CHECKSUM_OFFSET`;
  - index-leaf pages refresh `MYLITE_STORAGE_FORMAT_INDEX_LEAF_CHECKSUM_OFFSET`;
  - maintained-root pages refresh
    `MYLITE_STORAGE_FORMAT_INDEX_ROOT_CHECKSUM_OFFSET`; and
  - index-branch pages refresh
    `MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHECKSUM_OFFSET`.
- `test_record_dirty_checksum_refresh()` immediately calls
  `test_checksum_page_family(page, checksum_offset)` to rediscover that same
  family before incrementing dirty-refresh family/source counters.
- Existing dirty-refresh source/family counters are diagnostic output only.
  Checksum bytes are written by the same zero-tail checksum calls after this
  accounting step.

## Design

Add a test-hook helper that records dirty checksum refresh counters from a
known `mylite_storage_test_checksum_page_family`.

Use that helper inside each typed branch of
`refresh_dirty_buffered_page_checksum_with_family()`. Keep the old
page-and-offset helper as a fallback wrapper for future callers that have not
already classified the page.

Do not change checksum refresh selection, checksum bytes, dirty-buffer
publication, append-buffer flush, maintained-root planning, journal validation,
or non-test builds.

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
refresh code shape.

## Tests And Verification Plan

- Extend the dirty checksum refresh counter self-test so the normal typed
  refresh path records the row family without a parser call while the fallback
  page-and-offset helper remains covered.
- Existing storage self-tests and storage-smoke coverage exercise
  dirty-buffer flushes, append-buffer flushes, rollback, and recovery.
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

- Dirty checksum refresh family/source counters stay unchanged.
- Checksum call-site counters, maintained-root decode sites, dirty-page flush
  counts, append-buffer flush counts, pressure-admission counts, and merge
  direct-write counts stay unchanged.
- Storage and embedded storage-engine smoke tests pass.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `415.22 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `372.08 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; `libmariadbd.a` was `33,989,146` bytes with `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed with `79.694 us/op` prepared insert step under variable host load.

Current `prepared-insert-components 1000 100000` counters:

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

- The known family passed to the counter must match the branch that will
  refresh the checksum. Keep the fallback page parser and use only fixed family
  constants in branches that have already matched the page type.

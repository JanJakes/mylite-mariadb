# Dirty Page Buffer Replacement Leaf Fill Counters

## Problem

Prepared-insert profiles now show that buffer-limit flush victims are mostly
high-fill dirty partial leaves, but the replacement table still only reports
page family and write site. In the latest 100,000-iteration run, dirty-buffer
replacement counters showed `66,392` dirty index-leaf replacements from
`insert_branch_index_leaf_entry`, but not whether those replacements are sparse
leaf rewrites, mid-fill rewrites, or near-full churn.

Without replacement fill evidence, pressure-policy and checksum-timing work can
only see the pages that leave the dirty buffer, not the shape of repeated
in-buffer leaf rewrites that happen before pressure publishes a victim.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite profiling hooks only:
  `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/tests/storage_test.c`, and
  `tools/mylite_perf_baseline.c`.
- `record_dirty_page_buffer_replacement_page()` already observes every rewrite
  of a page already resident in the dirty page buffer, including page family,
  checksum-dirty state, and maintained write site.
- The existing leaf fill-band metadata validation used for flush victims can
  classify replacement leaf pages without decoding entries or refreshing
  checksums.

## Design

Add test-hook counters for dirty-page buffer replacement index-leaf fill bands:

- invalid;
- empty;
- 1-24%;
- 25-49%;
- 50-74%;
- 75-99%;
- full.

The benchmark prints the replacement fill-band table next to the existing
replacement family and write-site tables. Non-leaf replacements remain covered
by the existing replacement page-family table.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, file-format, or durable bytes change. The counters are
test-hook-only and are emitted by the local performance baseline tool.

## Single-File And Lifecycle Impact

No files are introduced. The counters observe existing in-memory dirty-buffer
replacement activity and do not change journal, rollback, flush, or file
lifecycle behavior.

## Binary Size And Dependency Impact

No new dependencies. Non-test-hook behavior is unchanged. Test-hook builds add a
small fixed counter array and accessors.

## Tests And Verification

- Add a storage test-hook case that replaces dirty-buffer pages with empty,
  partially filled, and full index leaves and verifies the replacement fill-band
  counters.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Replacement fill-band counters reset with prepared-insert profiling counters.
- Index-leaf replacements are counted by fill band.
- Existing replacement family/write-site, flush, pressure, checksum, and
  leaf-shape counters still report correctly.
- The prepared-insert benchmark prints the new replacement fill-band table.

## Risks

- Replacement fill bands describe the rewritten page shape, not whether that
  page will become a later pressure victim. The table is evidence for follow-up
  work, not a selector by itself.

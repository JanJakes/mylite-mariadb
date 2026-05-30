# Dirty Page Buffer Publication Checksum Sources

## Problem

Prepared-insert checksum output currently has a `dirty-page-flush` refresh
source that combines several publication paths: buffer-limit dirty-page flushes,
statement-commit dirty-page flushes, and merge direct writes. Recent merge
direct-write slices changed the balance between dirty-buffer pressure and
direct publication, so follow-up policy work needs checksum-refresh evidence
that separates those publication paths without changing durability behavior.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite test-hook observability only:
  `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/tests/storage_test.c`, and
  `tools/mylite_perf_baseline.c`.
- `write_dirty_page_buffer_entry()` refreshes checksum-dirty buffered pages
  with `MYLITE_STORAGE_DIRTY_CHECKSUM_REFRESH_SOURCE_DIRTY_PAGE_FLUSH` before
  writing them to the primary file.
- `flush_dirty_page_buffer_entry()` writes one buffered page for buffer-limit
  pressure and passes a `mylite_storage_dirty_page_buffer_flush_source`.
- `flush_statement_dirty_page_buffer()` writes all remaining dirty-buffer pages
  at statement-level flush points, including statement commit and test hooks.
- `direct_write_dirty_page_buffer_merge_entry()` writes a copied child dirty
  page through the same `write_dirty_page_buffer_entry()` helper when a merge
  direct-write guard accepts publication.

## Design

Add test-hook-only publication checksum counters keyed by publication path and
page family:

- `buffer-limit-flush`;
- `statement-commit-flush`;
- `test-hook-flush`;
- `merge-direct-write`; and
- `unknown`.

The existing dirty checksum refresh source counters remain unchanged. The new
counter records only checksum-dirty pages that are refreshed successfully by
the dirty-page publication path before write. Benchmark output prints sparse
non-zero rows beside the existing dirty-refresh source/family tables.

Use the existing flush source enum to map dirty-buffer flushes to publication
sources. Mark direct merge writes explicitly at the direct-write call site.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, file-format, durable bytes, or write policy changes. The
slice only adds test-hook counters and local benchmark output.

## Single-File And Lifecycle Impact

No files are introduced. Journal protection, rollback, direct-write guards,
dirty-buffer pressure, statement commit, and embedded lifecycle behavior remain
unchanged.

## Binary Size And Dependency Impact

No new dependencies. Non-test-hook behavior is unchanged. Test-hook builds add
a small fixed counter matrix and benchmark accessors.

## Tests And Verification

- Add a focused storage self-test that writes checksum-dirty index-leaf pages
  through:
  - buffer-limit dirty-buffer flush;
  - statement-commit dirty-buffer flush; and
  - merge direct write.
- Assert the existing `dirty-page-flush` refresh counter remains populated and
  the new publication-source/family counter records the expected source rows.
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

- Dirty-page publication checksum refreshes reset with prepared-insert profile
  counters.
- Benchmark output separates dirty-page checksum refreshes by buffer-limit
  flush, statement-commit flush, test-hook flush, merge direct write, and page
  family.
- Existing dirty checksum refresh counters, direct-write guard counters, dirty
  flush counters, rollback behavior, and checksum-dirty semantics remain
  unchanged.

## Risks

- These counters are publication-path evidence, not a new durability rule. They
  must not change which pages are refreshed or written.
- The existing `dirty-page-flush` refresh source remains intentionally broad
  for continuity with prior profiles; the new table is a split view for
  follow-up merge direct-write and pressure work.

## Verification Evidence

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  passed in 283.27 seconds after formatting.
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  rebuilt `libmariadbd.a` at 32.40 MiB and `33,974,930` bytes.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed in 296.62 seconds.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  reported the prepared insert step at `69.789 us/op`; the existing
  `dirty-page-flush` refresh bucket remained `88,170`, split by publication
  source into `32,266` buffer-limit `index-leaf` refreshes, `1`
  statement-commit `index-leaf` refresh, `1` statement-commit `index-branch`
  refresh, and `55,902` merge-direct-write `index-leaf` refreshes.

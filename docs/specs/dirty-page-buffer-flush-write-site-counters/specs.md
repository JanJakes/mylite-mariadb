# Dirty Page Buffer Flush Write-Site Counters

## Problem

Prepared-insert smoke output now identifies two dirty-page buffer pressure
views: the incoming page that forced pressure, and replacements of pages
already resident in the buffer. The remaining high-volume pressure signal is
the page actually flushed by buffer-limit pressure. Current output reports the
flush source and page family, but not the maintained writer that originally
buffered the flushed page.

The latest local prepared-insert component run after dirty-page buffer buckets
still reports `54,529` buffer-limit flushes, all `index-leaf` pages, and
`insert_branch_index_leaf_entry` as the dominant incoming and replacement write
site. Flush-victim write-site attribution is the next narrow evidence step
before changing pressure policy or dirty-buffer ownership.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage and the first-party performance
  tool only:
  `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/tests/storage_test.c`, and
  `tools/mylite_perf_baseline.c`.
- Dirty-page buffer entries already carry a test-hook-only
  `pressure_write_site_name`. That name is set when maintained insert writers
  call `pager_write_maintained_insert_page()` and is preserved when nested
  dirty buffers merge into their parent.
- `record_dirty_page_buffer_flush_page()` already receives both the flush source
  and the full dirty-page buffer entry before `write_dirty_page_buffer_entry()`
  clears any checksum-dirty state. This is the right point to count the flushed
  page's family, source, dirty state, and write site without changing behavior.

## Design

Add test-hook-only flush write-site counters:

- maintain a bounded slot table keyed by `pressure_write_site_name`;
- count flushed pages by `(site, flush source, page family)`;
- count checksum-dirty flushed pages by the same dimensions;
- expose getter functions for the performance baseline;
- print a prepared-insert table showing site, source, family, flush pages, and
  checksum-dirty flush pages;
- reset counters through the existing prepared-insert profile reset.

The counters are intentionally diagnostic only. They do not affect dirty-page
buffer pressure selection, page publication, checksum refresh, nested
statement merge, or rollback semantics.

## Compatibility Impact

No SQL, public C API, handler API, storage-engine routing, file-format, or DDL
metadata behavior changes. The new counters are available only in test-hook
builds and are consumed by the local performance baseline.

## Single-File And Lifecycle Impact

No new files are introduced. Dirty pages still flush through the existing
primary `.mylite` write path and existing recovery/transaction journal
protection.

## Binary Size And Dependency Impact

No new dependencies. Non-test-hook builds retain no flush write-site tables.
Test-hook builds add bounded in-memory counter arrays and small getter
functions.

## Tests And Verification

- Add a storage test-hook case that fills a dirty-page buffer with dirty
  maintained index leaves tagged by a write site, triggers buffer-limit
  pressure, and verifies the flushed page is counted under that site, source,
  page family, and checksum-dirty state.
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

- Flush write-site counters report buffer-limit victims by site, source, family,
  and checksum-dirty state in test-hook builds.
- Existing dirty-page buffer flush, pressure, replacement, and checksum counters
  remain intact.
- Prepared-insert benchmark output includes a non-empty flush write-site table
  for the hot maintained-index pressure path.
- Storage and embedded storage-engine smoke tests pass.

## Risks

- Counter dimensions can grow noisy if many call sites participate. The bounded
  slot table follows the existing pressure and replacement write-site counter
  model and drops excess sites rather than growing unbounded state.
- These counters identify pressure victims but do not by themselves reduce the
  `index-leaf` flush count. A later pressure-policy slice should use this
  evidence before changing eviction order.

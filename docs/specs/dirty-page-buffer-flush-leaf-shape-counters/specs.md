# Dirty Page Buffer Flush Leaf Shape Counters

## Problem

The prepared-insert benchmark still attributes nearly every buffer-limit dirty
page flush to maintained index leaves written by `insert_branch_index_leaf_entry`.
After adding a pressure preference for structurally full dirty leaves, the
existing counters can show the flush source, page family, and write site, but
they cannot show whether the flushed leaves are clean, dirty partial leaves, or
dirty full leaves.

That leaves the next pressure-policy decision under-evidenced. If pressure is
mostly flushing full dirty leaves, the full-leaf preference is selecting the
intended class and follow-up work should reduce publication cost. If pressure is
still flushing many partial dirty leaves, the selector needs a deeper strategy.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite profiling hooks only:
  `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/tests/storage_test.c`, and
  `tools/mylite_perf_baseline.c`.
- `record_dirty_page_buffer_flush_page()` already records flush source, page
  family, checksum-dirty state, and maintained write site for dirty-buffer
  flushes.
- `dirty_page_buffer_entry_is_full_index_leaf()` already has the conservative
  fixed-width leaf metadata check needed to classify full dirty leaves without
  decoding entries or refreshing checksums.

## Design

Add test-hook counters for dirty-page buffer leaf flush shape by flush source:

- clean leaf;
- dirty partial leaf;
- dirty full leaf.

Only index leaf pages are classified. Other page families stay covered by the
existing flush family table. The benchmark prints the new table after the
existing flush family table so source totals can be compared directly.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, file-format, or durable page bytes change. The new counters
are only compiled under MyLite storage test hooks and only emitted by the local
performance baseline tool.

## Single-File And Lifecycle Impact

No files are introduced. The counters observe existing flushes to the primary
`.mylite` file and do not change journal, rollback, or flush ordering.

## Binary Size And Dependency Impact

No new dependencies. Non-test-hook builds are unchanged. Test-hook builds add a
small fixed counter matrix and accessor functions.

## Tests And Verification

- Add a storage test-hook case that buffers and flushes one clean leaf, one
  dirty partial leaf, and one dirty full leaf, then verifies the new counters by
  flush source.
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

- Flush leaf shape counters reset with the prepared-insert profiling counters.
- Clean, dirty partial, and dirty full index leaf flushes are counted by flush
  source.
- Non-leaf flushes remain represented by the existing page-family table and do
  not affect leaf shape counters.
- The prepared-insert benchmark prints the new table.

## Risks

- The full-leaf classification follows fixed-width leaf metadata and should be
  updated if the index leaf format changes. Malformed or ambiguous dirty leaves
  are counted as dirty partial leaves because the full check is conservative.

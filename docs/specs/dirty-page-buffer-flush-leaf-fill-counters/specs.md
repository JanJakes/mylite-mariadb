# Dirty Page Buffer Flush Leaf Fill Counters

## Problem

Prepared-insert flush leaf-shape counters show that buffer-limit pressure mostly
flushes dirty partial maintained index leaves. A follow-up dense-leaf pressure
experiment was rejected because it increased buffer-limit dirty leaf flushes,
which means entry count alone is not enough to select colder dirty leaves.

The benchmark still needs more evidence about those partial leaves. The current
shape table can distinguish full from partial leaves, but it cannot show whether
partial victims are nearly empty, moderately filled, or close to full.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite profiling hooks only:
  `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/tests/storage_test.c`, and
  `tools/mylite_perf_baseline.c`.
- `record_dirty_page_buffer_flush_page()` already observes every dirty-buffer
  flush victim with source, page family, write site, and leaf shape.
- Fixed-width index leaf metadata has enough information to classify fill
  ratio conservatively: key size, entry count, capacity, and used bytes.

## Design

Add test-hook counters for dirty-page buffer index-leaf flush fill bands by
flush source:

- invalid;
- empty;
- 1-24%;
- 25-49%;
- 50-74%;
- 75-99%;
- full.

The metadata check is conservative. A leaf is counted as `invalid` when the
fixed-width metadata does not match the expected page layout. Non-leaf flushes
remain represented only by the existing page-family table.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, file-format, or durable bytes change. The counters are
test-hook-only and are emitted by the local performance baseline tool.

## Single-File And Lifecycle Impact

No files are introduced. The counters observe existing flushes to the primary
`.mylite` file and do not change journal, rollback, or flush ordering.

## Binary Size And Dependency Impact

No new dependencies. Non-test-hook builds only retain the existing full-leaf
helper behavior through a small metadata helper. Test-hook builds add a fixed
counter matrix and accessors.

## Tests And Verification

- Add a storage test-hook case that flushes empty, partially filled, and full
  index leaves and verifies the fill-band counters by flush source.
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

- Fill-band counters reset with prepared-insert profiling counters.
- Index-leaf flushes are counted by source and fill band.
- Existing leaf-shape, flush write-site, pressure, replacement, and checksum
  counters still report correctly.
- The prepared-insert benchmark prints the new fill-band table.

## Risks

- Fill bands describe occupancy, not future hotness. They are evidence for the
  next selector design, not a selector by themselves.

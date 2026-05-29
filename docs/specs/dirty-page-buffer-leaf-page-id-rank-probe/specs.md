# Dirty Page Buffer Leaf Page-Id Rank Probe

## Problem

The prepared-insert smoke profile after `e2ddbe94` still reports `53,997`
buffer-limit index-leaf flushes. Fill-band counters show most dirty leaf
victims are near full, but leaf pages do not currently report whether pressure
is publishing the newest/highest page-id leaf in the buffered leaf set. Without
that evidence, choosing higher-fill dirty leaves risks evicting the hot append
edge in ascending insert workloads.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test-hook code only:
  `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/tests/storage_test.c`, and
  `tools/mylite_perf_baseline.c`.
- `dirty_page_buffer_pressure_flush_index()` already chooses one resident page
  when the fixed dirty-page buffer reaches
  `MYLITE_STORAGE_DIRTY_PAGE_BUFFER_LIMIT`.
- `record_dirty_page_buffer_flush_page()` already reports flushed page family,
  leaf shape, fill band, and write site by flush source in test-hook builds.
- The index leaf page format stores page id, table/index id, key size, entry
  count, and used bytes, but no leaf sibling pointers. A probe can therefore
  classify the flushed leaf only relative to the resident buffered leaves.

## Design

Add test-hook counters that classify each flushed index leaf as:

- `max-leaf-page-id`: the flushed leaf has the highest page id among resident
  buffered index leaves at the time of the flush;
- `non-max-leaf-page-id`: at least one resident buffered index leaf has a
  higher page id; or
- `invalid`: reserved for malformed probe input.

The probe runs only while `test_count_checksum_page_calls` is enabled. It scans
the fixed-size dirty-page buffer before the target slot is reused, adds no
production-build work, and reports counts by existing dirty-page flush source.

## Compatibility Impact

No SQL behavior, C API behavior, storage-engine routing, metadata, file format,
or durable bytes change. The slice only adds test-hook counters and local
benchmark output.

## Single-File And Lifecycle Impact

No files are introduced. Dirty pages still flush through the existing
journal-protected primary `.mylite` path.

## Binary Size And Dependency Impact

No dependencies are added. Non-test-hook builds are unaffected because the probe
is compiled under `MYLITE_STORAGE_TEST_HOOKS`.

## Tests And Verification

- Add storage test-hook coverage for max and non-max leaf page-id rank counts.
- Expose the counters in `tools/mylite_perf_baseline.c`.
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

- Buffer-limit and direct test-hook flushes count max vs non-max buffered leaf
  page-id rank correctly.
- Existing flush family, shape, fill-band, write-site, replacement, and
  checksum counters still reset and report correctly.
- Prepared-insert benchmark output includes the new rank table.
- Storage and embedded storage-engine tests pass.

## Verification Evidence

On the VPS on 2026-05-29:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`: passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `334.41 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`: passed; archive
  `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a`,
  `32.40 MiB`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`: passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`: passed
  in `318.02 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`: passed; prepared insert step
  component was `81.534 us/op`.

The prepared-insert benchmark reported `53,997` buffer-limit index-leaf flushes.
The new page-id rank table classified those leaf victims as `50,579`
`non-max-leaf-page-id` and `3,418` `max-leaf-page-id`.

## Risks

- Highest page id is evidence for append-edge behavior in current allocation
  patterns, not a durable index-order invariant. Follow-up eviction changes
  must treat the metric as workload evidence rather than a semantic proof.

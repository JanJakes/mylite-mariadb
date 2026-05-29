# Dirty Page Buffer Leaf Rank Fill-Band Probe

## Problem

The leaf page-id rank probe shows the prepared-insert pressure path flushes
both `non-max-leaf-page-id` and `max-leaf-page-id` victims, while the existing
fill-band table shows most dirty leaf victims are in the `75-99%` band. Those
tables are separate, so they do not show whether near-full victims are mostly
append-edge candidates or older buffered leaves.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite test-hook and benchmark code only:
  `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/tests/storage_test.c`, and
  `tools/mylite_perf_baseline.c`.
- `record_dirty_page_buffer_flush_leaf_fill_band()` already classifies flushed
  leaf density by source.
- `record_dirty_page_buffer_flush_leaf_page_id_rank()` already classifies
  flushed leaves as max or non-max page id among resident buffered leaves.

## Design

Add a test-hook matrix counter keyed by dirty-page flush source, leaf page-id
rank, and leaf fill band. The matrix reuses the same page-id rank and fill-band
definitions as the existing one-dimensional tables, runs only while test-hook
profiling is enabled, and adds no production-build work.

`tools/mylite_perf_baseline` will print only non-zero matrix rows to keep the
prepared-insert output focused.

## Compatibility Impact

No SQL behavior, public C API behavior, storage-engine routing, metadata, file
format, or durable bytes change. The slice only adds test-hook counters and
benchmark output.

## Single-File And Lifecycle Impact

No files are introduced. Dirty pages still flush through the existing
journal-protected primary `.mylite` path.

## Binary Size And Dependency Impact

No dependencies are added. Non-test-hook builds are unaffected because the
matrix is compiled under `MYLITE_STORAGE_TEST_HOOKS`.

## Tests And Verification

- Add storage test-hook coverage for max and non-max page-id rank combined with
  empty, mid-fill, and full leaf bands.
- Expose non-zero matrix rows in `tools/mylite_perf_baseline.c`.
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

- Direct and buffer-limit test-hook flushes populate the rank/fill-band matrix
  with the same rank and fill-band definitions used by the existing tables.
- Existing rank, fill-band, shape, write-site, replacement, and checksum
  counters still reset and report correctly.
- Prepared-insert benchmark output includes non-zero rank/fill-band rows.
- Storage and embedded storage-engine tests pass.

## Verification Evidence

On the VPS on 2026-05-29:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`: passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `309.58 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`: passed; archive
  `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a`,
  `32.40 MiB`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`: passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`: passed
  in `312.27 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`: passed; prepared insert step
  component was `90.236 us/op`.

The prepared-insert rank/fill-band matrix reported:

- `non-max-leaf-page-id`: `63` in `1-24%`, `629` in `25-49%`,
  `8,062` in `50-74%`, `38,947` in `75-99%`, and `2,878` full.
- `max-leaf-page-id`: `628` in `1-24%`, `90` in `25-49%`,
  `2,194` in `50-74%`, `492` in `75-99%`, and `14` full.

## Risks

- The matrix is evidence for current allocation and insert patterns, not a
  semantic proof of index edge position. Follow-up production heuristics must
  continue to treat max page id as a workload proxy only.

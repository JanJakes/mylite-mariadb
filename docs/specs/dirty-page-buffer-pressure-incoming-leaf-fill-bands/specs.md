# Dirty Page Buffer Pressure Incoming Leaf Fill Bands

## Problem

The prepared-insert smoke profile now shows that buffer-limit flush victims are
mostly high-occupancy, first-admitted index leaves: `69,547` never-replaced
victims in the `75-99%` fill band and `3,025` never-replaced full leaves.
Pressure counters also show the pages admitted after those flushes are mostly
checksum-dirty index leaves (`85,257`) with a small number of branch pages
(`275`), but the benchmark does not report the occupancy of those incoming
leaf pages.

Without incoming-leaf fill-band evidence, the next pressure-policy or checksum
slice cannot tell whether the fixed dirty-page window is being refilled with
the same high-occupancy leaves it is evicting, or whether victims become
high-fill only after later in-buffer rewrites.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test hooks and benchmark
  reporting in `packages/mylite-storage/src/storage.c` and
  `tools/mylite_perf_baseline.c`.
- `store_dirty_page_in_buffer()` records pressure admissions immediately after
  a buffer-limit flush through `record_dirty_page_buffer_pressure_incoming_page()`.
- The same test-hook fill-band enum and conservative fixed-width leaf metadata
  validation already classify flushed and replacement index leaves.
- The probe is MyLite-owned observability; it does not depend on upstream
  MariaDB storage-engine behavior.

## Design

Add a test-hook-only incoming pressure leaf fill-band counter:

- reuse `dirty_page_buffer_leaf_fill_band()` so incoming, flushed, and
  replacement leaf tables share one classification;
- increment the counter from
  `record_dirty_page_buffer_pressure_incoming_page()` only for incoming index
  leaves;
- expose an accessor for benchmark reporting; and
- print a prepared-insert table for incoming leaf fill bands.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, file-format, durable bytes, or supported/unsupported surface
changes. The new counters exist only when `MYLITE_STORAGE_TEST_HOOKS` is
enabled.

## Single-File And Lifecycle Impact

No files are introduced. The probe does not change dirty-page buffer capacity,
eviction order, journal protection, nested-statement merge, rollback, page
publication, or checksum refresh behavior.

## Binary Size And Dependency Impact

No new dependencies. Production builds without storage test hooks are
unchanged.

## Tests And Verification

- Add storage test-hook coverage proving pressure admissions classify incoming
  index leaves by fill band without affecting existing family and write-site
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

## Verification Evidence

The VPS storage-smoke benchmark reported a `71.091 us/op` prepared insert step
with these incoming pressure leaf fill bands:

| Leaf fill band | Incoming pages |
| --- | ---: |
| invalid | 0 |
| empty | 0 |
| 1-24% | 16 |
| 25-49% | 0 |
| 50-74% | 6,920 |
| 75-99% | 75,020 |
| full | 3,301 |

The incoming family counters remained consistent with the new table:
`85,257` incoming index leaves and `275` incoming index branches, with
`85,257` checksum-dirty incoming index leaves.

## Acceptance Criteria

- Prepared-insert benchmark output reports incoming pressure leaf fill bands.
- Existing pressure incoming family/write-site counters still report correctly.
- Existing flush, replacement, checksum, storage, and embedded storage-engine
  smoke tests pass.

## Risks

- The counter must stay coupled to the existing fill-band helper. Divergent
  classification would make incoming/victim comparisons misleading.

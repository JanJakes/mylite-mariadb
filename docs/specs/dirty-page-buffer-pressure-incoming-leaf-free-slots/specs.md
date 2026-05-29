# Dirty Page Buffer Pressure Incoming Leaf Free Slots

## Problem

The prepared-insert pressure profile now shows that incoming pressure leaves
are mostly high-fill: `75,020` incoming leaves in the `75-99%` fill band and
`3,301` full incoming leaves. That proves the dirty-page buffer is refilled
with high-occupancy leaves, but the `75-99%` band is still too coarse for a
follow-up pressure policy. A leaf with one free slot and a leaf with many
remaining slots both fall into the same bucket.

Before changing whether nearly-full incoming leaves should stay in the dirty
buffer or take the direct-write fallback, the benchmark needs exact
remaining-capacity buckets for those incoming pressure leaves.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test hooks and benchmark
  reporting in `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/tests/storage_test.c`, and
  `tools/mylite_perf_baseline.c`.
- `record_dirty_page_buffer_pressure_incoming_page()` records the incoming
  page after a buffer-limit flush and already sees the page bytes before the
  dirty-buffer slot is reused.
- `dirty_page_buffer_index_leaf_fill()` validates fixed-width leaf metadata and
  returns entry count plus entry capacity without requiring checksum refresh.
- Existing incoming fill-band counters show occupancy shape, but not the
  remaining slot count needed for a direct-write or bypass threshold.

## Design

Add a test-hook-only incoming pressure leaf free-slot counter:

- classify incoming index leaves into invalid, zero, one, two-to-three,
  four-to-seven, eight-to-fifteen, and sixteen-plus free-slot buckets;
- reuse `dirty_page_buffer_index_leaf_fill()` so the classification is tied to
  the same conservative metadata validation as existing fill-band counters;
- expose slot-count, slot-name, and count accessors for benchmark output; and
- print the table next to the existing incoming pressure fill-band table.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, file-format, durable bytes, or supported/unsupported surface
changes. The new counters exist only when `MYLITE_STORAGE_TEST_HOOKS` is
enabled.

## Single-File And Lifecycle Impact

No files are introduced. The probe does not change dirty-page buffer capacity,
eviction order, direct-write fallback behavior, journal protection, rollback,
nested-statement merge, page publication, or checksum refresh behavior.

## Binary Size And Dependency Impact

No new dependencies. Production builds without storage test hooks are
unchanged.

## Tests And Verification

- Add storage test-hook coverage proving invalid, zero, one, two-to-three,
  four-to-seven, eight-to-fifteen, and sixteen-plus free-slot buckets are
  counted.
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

The VPS storage-smoke benchmark reported a `78.551 us/op` prepared insert step
with these incoming pressure leaf free-slot buckets:

| Leaf free slots | Incoming pages |
| --- | ---: |
| invalid | 0 |
| 0 | 3,301 |
| 1 | 3,043 |
| 2-3 | 5,048 |
| 4-7 | 7,840 |
| 8-15 | 11,864 |
| 16+ | 54,161 |

The free-slot table totals `85,257` incoming index leaves, matching the
incoming family counter. The table shows `31,096` incoming pressure leaves have
at most `15` free slots, while the majority of incoming leaves still have
`16+` free slots even though they fall mostly in the `75-99%` fill band.

## Acceptance Criteria

- Prepared-insert benchmark output reports incoming pressure leaf free-slot
  buckets.
- Existing incoming family, incoming fill-band, pressure write-site, flush,
  replacement, and checksum counters still report correctly.
- Storage and embedded storage-engine smoke tests pass.

## Risks

- Free-slot buckets are evidence for a later policy, not a policy by
  themselves. A later direct-write threshold still needs a benchmarked
  before/after slice and rollback coverage.

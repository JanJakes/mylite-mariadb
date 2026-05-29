# Dirty Page Buffer Flush Leaf Free Slots

## Problem

Prepared-insert pressure profiles still report `85,532` buffer-limit
index-leaf flushes and matching `dirty-page-flush` checksum refreshes. Existing
flush leaf fill-band counters show those victims are mostly high-fill, but the
`75-99%` band is too broad for a follow-up direct-write or bypass threshold.
The incoming pressure path now reports exact remaining free-slot buckets; the
flush side needs the same shape evidence for the leaves that actually incur
pressure publication work.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test hooks and benchmark
  reporting in `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/tests/storage_test.c`, and
  `tools/mylite_perf_baseline.c`.
- `flush_statement_dirty_page_buffer()` and `flush_dirty_page_buffer_entry()`
  record flush-source and page-family counters before publishing dirty-buffer
  entries.
- `record_dirty_page_buffer_flush_page()` already records index-leaf shape,
  fill band, replacement state, and maintained write-site attribution for each
  flushed entry.
- `dirty_page_buffer_index_leaf_fill()` validates fixed-width leaf metadata and
  returns entry count plus entry capacity without changing page bytes.

## Design

Add a test-hook-only flush leaf free-slot counter:

- classify flushed index leaves into invalid, zero, one, two-to-three,
  four-to-seven, eight-to-fifteen, and sixteen-plus free-slot buckets;
- reuse the same conservative free-slot helper used by incoming pressure leaf
  counters;
- store counts by dirty-page buffer flush source so buffer-limit pressure,
  statement commit, and test-hook flushes remain distinguishable;
- expose slot-count, slot-name, and count accessors for benchmark output; and
- print the flush free-slot table next to the existing flush fill-band table.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, file-format, durable bytes, or supported/unsupported surface
changes. The new counters exist only when `MYLITE_STORAGE_TEST_HOOKS` is
enabled.

## Single-File And Lifecycle Impact

No files are introduced. The probe does not change dirty-page buffer capacity,
victim selection, checksum refresh timing, journal protection, rollback,
nested-statement merge, page publication, or embedded open/close behavior.

## Binary Size And Dependency Impact

No new dependencies. Production builds without storage test hooks are
unchanged.

## Tests And Verification

- Add storage test-hook coverage proving invalid, zero, one, two-to-three,
  four-to-seven, eight-to-fifteen, and sixteen-plus flushed leaf free-slot
  buckets are counted for the test-hook flush source.
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

The VPS storage-smoke benchmark reported an `80.423 us/op` prepared insert step
with these buffer-limit flush leaf free-slot buckets:

| Leaf free slots | buffer-limit | statement-commit | test-hook |
| --- | ---: | ---: | ---: |
| invalid | 0 | 0 | 0 |
| 0 | 3,327 | 0 | 0 |
| 1 | 3,046 | 0 | 0 |
| 2-3 | 5,078 | 0 | 0 |
| 4-7 | 7,838 | 0 | 0 |
| 8-15 | 11,918 | 0 | 0 |
| 16+ | 54,325 | 0 | 0 |

The free-slot table totals `85,532` buffer-limit index-leaf flushes, matching
the existing flush source and family counters. It shows `27,880` flushed
pressure victims have `1-15` free slots and `54,325` still have `16+` free
slots even though the flush fill-band table places most victims in the
`75-99%` band.

## Acceptance Criteria

- Prepared-insert benchmark output reports flushed index-leaf free-slot buckets
  by flush source.
- Existing flush source, flush family, flush fill-band, pressure incoming,
  replacement, write-site, and checksum counters still report correctly.
- Storage and embedded storage-engine smoke tests pass.

## Risks

- Free-slot buckets are evidence for a later policy, not a policy by
  themselves. A later direct-write or bypass threshold still needs a
  benchmarked before/after slice and rollback coverage.

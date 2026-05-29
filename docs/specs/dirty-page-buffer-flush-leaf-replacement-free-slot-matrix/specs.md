# Dirty Page Buffer Flush Leaf Replacement Free Slot Matrix

## Problem

The prepared-insert pressure profile now reports exact free-slot buckets for
flushed index-leaf victims and separately reports whether each flushed leaf was
never replaced, replaced once, or replaced multiple times in the dirty-page
buffer. Those tables are still separate. A direct-write or bypass threshold
needs to know whether near-full victims were first-admitted pages that did not
benefit from dirty-buffer coalescing, or pages that were rewritten while
resident and should keep using the buffer.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test hooks, storage self-test
  coverage, and benchmark reporting in
  `packages/mylite-storage/src/storage.c` and
  `tools/mylite_perf_baseline.c`.
- `record_dirty_page_buffer_flush_leaf_replacement_state()` already sees the
  flushed dirty-buffer entry, its source, and its replacement count.
- `dirty_page_buffer_leaf_free_slot_band()` validates fixed-width leaf
  metadata and classifies remaining capacity without changing page bytes.
- Existing replacement-state/fill-band output proves most victims are
  high-fill and never replaced, but does not distinguish leaves with one free
  slot from leaves with many remaining slots.

## Design

Add a test-hook-only replacement-state/free-slot matrix for flushed index
leaves:

- reuse existing replacement-state classification;
- reuse existing free-slot bucket classification;
- record counts by flush source, replacement state, and free-slot bucket;
- print only nonzero matrix rows in the prepared-insert benchmark; and
- keep the existing standalone replacement-state, fill-band, free-slot,
  pressure, and checksum counters unchanged.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, file-format, durable bytes, or supported/unsupported surface
changes. The new matrix exists only when `MYLITE_STORAGE_TEST_HOOKS` is
enabled.

## Single-File And Lifecycle Impact

No files are introduced. The probe does not change dirty-page buffer capacity,
victim selection, direct-write fallback behavior, journal protection,
rollback, nested-statement merge, page publication, checksum refresh timing, or
embedded open/close behavior.

## Binary Size And Dependency Impact

No new dependencies. Production builds without storage test hooks are
unchanged.

## Tests And Verification

- Add storage test-hook coverage proving the matrix records flushed index
  leaves across replacement states and free-slot buckets.
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

The VPS storage-smoke benchmark reported a `71.920 us/op` prepared insert step
with these buffer-limit replacement-state/free-slot rows:

| Replacement state | 0 | 1 | 2-3 | 4-7 | 8-15 | 16+ |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| never-replaced | 3,025 | 2,793 | 4,634 | 7,186 | 11,022 | 50,261 |
| replaced-once | 232 | 194 | 329 | 472 | 628 | 2,434 |
| replaced-multiple | 70 | 59 | 115 | 180 | 268 | 1,630 |

The matrix totals `85,532` buffer-limit index-leaf flushes, matching the
existing flush source, free-slot, and replacement-state counters. For the
`0-15` free-slot range, `28,660` victims were never replaced, `1,855` were
replaced once, and `692` were replaced multiple times before publication.

## Acceptance Criteria

- Prepared-insert benchmark output reports flushed index-leaf replacement
  state by free-slot buckets.
- Existing flush source, flush family, flush fill-band, flush free-slot,
  replacement-state/fill-band, pressure incoming, replacement, write-site, and
  checksum counters still report correctly.
- Storage and embedded storage-engine smoke tests pass.

## Risks

- The matrix is evidence for a later policy, not a policy by itself. A direct
  write or bypass threshold still needs rollback coverage and before/after
  benchmark evidence.

# Dirty Page Buffer Flush Leaf Replacement Fill Matrix

## Problem

The replacement-state probe shows the latest prepared-insert smoke profile
publishes `78,921` buffer-limit index leaves that were never rewritten after
dirty-buffer admission. Existing fill-band counters show most flushed leaves
are near-full, but the benchmark does not yet join replacement state with leaf
occupancy.

That missing cross-table leaves the next checksum or pressure slice guessing
whether first-admitted flush work is dominated by sparse leaves, mid-fill
leaves, near-full leaves, or structurally full leaves.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite test-hook storage and benchmark code:
  `packages/mylite-storage/src/storage.c` and
  `tools/mylite_perf_baseline.c`. It extends storage self-test coverage that
  is exercised by `packages/mylite-storage/tests/storage_test.c`.
- `record_dirty_page_buffer_flush_leaf_fill_band()` already classifies flushed
  leaf occupancy.
- `record_dirty_page_buffer_flush_leaf_replacement_state()` already
  classifies flushed leaves as never, once, or multiply replaced.
- Both classifications are test-hook-only observability and do not affect the
  production pressure selector or dirty-page publication.

## Design

Add a test-hook-only source/state/fill-band counter matrix for flushed index
leaves:

- reuse the existing conservative fixed-width leaf metadata check for fill
  bands;
- reuse the existing per-entry replacement counter for replacement state;
- expose an accessor for benchmark reporting; and
- print non-zero rows in the prepared-insert component benchmark.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, file-format, durable bytes, or supported/unsupported surface
changes. The new counters exist only when `MYLITE_STORAGE_TEST_HOOKS` is
enabled.

## Single-File And Lifecycle Impact

No files are introduced. The probe does not change flush order, dirty-page
publication, journal protection, rollback, nested statement merge, or checksum
refresh behavior.

## Binary Size And Dependency Impact

No new dependencies. Production builds are unchanged.

## Tests And Verification

- Add storage test-hook coverage proving the matrix records never-replaced,
  replaced-once, and replaced-multiple leaves in distinct fill bands.
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Verification Evidence

The VPS storage-smoke benchmark reported a `71.898 us/op` prepared insert step
with the following non-zero buffer-limit replacement-state/fill-band rows:

| Leaf replacement state | Leaf fill band | Flush pages |
| --- | --- | ---: |
| never-replaced | 50-74% | 6,349 |
| never-replaced | 75-99% | 69,547 |
| never-replaced | full | 3,025 |
| replaced-once | 50-74% | 131 |
| replaced-once | 75-99% | 3,926 |
| replaced-once | full | 232 |
| replaced-multiple | 50-74% | 247 |
| replaced-multiple | 75-99% | 2,005 |
| replaced-multiple | full | 70 |

The aggregate replacement-state and fill-band counters still match the matrix
totals: `78,921` never-replaced, `4,289` replaced-once, `2,322`
replaced-multiple, `6,727` in `50-74%`, `75,478` in `75-99%`, and `3,327`
full buffer-limit leaf flushes.

## Acceptance Criteria

- Benchmark output reports non-zero flushed-leaf replacement-state/fill-band
  rows.
- Existing replacement-state, fill-band, rank, pressure, replacement, and
  checksum counters still report correctly.
- Storage and embedded storage-engine smoke tests pass.

## Risks

- The matrix must use the same fill-band logic as existing flush counters. If
  those classifications diverge, benchmark output will become misleading.

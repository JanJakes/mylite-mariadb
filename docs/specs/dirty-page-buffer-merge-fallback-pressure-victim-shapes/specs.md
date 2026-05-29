# Dirty-Page Buffer Merge Fallback Pressure Victim Shapes

## Problem

The previous pressure-victim evidence joined rejected below-tail fallback
admissions to the page family and replacement state of the buffer-limit victim.
That proved the rejected candidates currently evict dirty index leaves, but it
still does not identify whether those victims are full leaves, broad partial
leaves, or the resident leaf tail.

Before changing direct-write or pressure-selection behavior, the prepared
insert profile needs to show the shape of the victim that the rejected
candidate displaced.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage test hooks and benchmark
  reporting only.
- `store_dirty_page_in_buffer_at_pressure_write_site()` knows the selected
  pressure slot before `flush_dirty_page_buffer_entry()` writes the victim and
  before the slot is reused for the incoming page.
- Existing test-hook helpers already classify dirty-buffer leaf fill bands,
  coarse and detailed free-slot bands, and max/non-max page-id rank for global
  flush output.
- The merge fallback pressure-victim counters already have the incoming
  fallback leaf context: guard outcome, parent tail-distance band, and incoming
  free-slot detail band.

## Design

Extend the merge fallback pressure-victim counters with victim leaf shape:

- victim leaf fill band;
- victim leaf free-slot detail band;
- victim leaf page-id rank among resident buffered leaves.

Refactor the existing page-id rank calculation into a small helper used by both
global flush counters and the merge fallback pressure-victim join. Keep the
helper under test-hook code and preserve the existing global flush output.

Add rejected below-tail summary accessors for the same failed direct-write
predicate:

- guard outcome is `future-current-header-partial-leaf`;
- parent leaf tail distance is `below-parent-max-by-32-127`;
- incoming leaf free-slot detail is `32-63` or `64-127`.

The prepared-insert benchmark prints compact rejected-candidate victim tables
for fill band, free-slot detail, and page-id rank.

## Compatibility Impact

No SQL behavior, public MyLite C API, handler API, storage-engine routing, or
file-format behavior changes. `ENGINE=InnoDB` continues to route through
MyLite. The new functions are test-hook-only benchmark helpers.

## Single-File And Lifecycle Impact

No durable state, sidecars, journal layout, rollback layout, or embedded
lifecycle changes. The counters are process-local test-hook state and are reset
with the existing prepared-insert profile reset path.

## Public API And File Format Impact

No public API or on-disk format changes. The new symbols are internal test-hook
accessors used by storage self-tests and the local benchmark tool.

## Storage-Engine Routing Impact

No routing change. Supported MySQL/MariaDB storage-engine names, including
`ENGINE=InnoDB`, continue to resolve through the MyLite storage layer.

## Binary-Size Impact

No new dependencies. The added code is limited to test-hook builds and the
benchmark tool. Counter storage is allocated lazily on the heap only when the
existing test-hook tensor is used.

## Tests And Verification

- Extend the focused dirty-page buffer merge fallback parent-rank/tail-distance
  storage self-test so the synthetic rejected below-tail candidate reports:
  - one empty victim leaf;
  - one `128+` free-slot victim leaf;
  - one max-page-id victim leaf.
- Extend the prepared-insert benchmark output with rejected below-tail
  pressure-victim shape summaries.
- Implementation evidence on `custom-storage`:
  - dev `mylite-storage` CTest passed in `302.30 sec`;
  - embedded static smoke build completed with `libmariadbd.a` at
    `33,974,138` bytes;
  - storage-smoke CTests passed, including `mylite-storage` in `289.04 sec`
    and `libmylite.embedded-storage-engine` in `14.66 sec`;
  - prepared-insert benchmark reported a `71.295 us/op` prepared insert step,
    `53,136` dirty leaf direct merge writes, and `34,484` dirty leaf pressure
    admissions;
  - rejected below-tail candidate admissions remained `11,971`, with all
    `11,971` pressure victims reported as checksum-dirty `index-leaf` pages;
  - rejected below-tail pressure-victim shape output reported all `11,971`
    victims as `non-max-leaf-page-id`, with fill bands `9,676` in `75-99%`,
    `2,292` in `50-74%`, and `3` full;
  - victim free-slot detail reported `6,258` in `32-63`, `5,172` in `64-127`,
    `454` in `128+`, and `87` below `32` free slots.
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

- Buffer-limit pressure caused by a merge fallback leaf records victim leaf
  fill band, free-slot detail, and page-id rank before the slot is overwritten.
- Rejected below-tail candidate summary accessors report those victim shape
  counts.
- Existing merge direct-write, fallback replay, flush, and pressure-selection
  behavior is unchanged.
- Focused storage tests and prepared-insert benchmark output cover the new
  evidence.

## Risks

- Page-id rank is only an observed dirty-buffer relation at pressure time, not
  a durable index-ordering invariant.
- The rejected-candidate summary intentionally hides detail that remains
  available in the full tail-distance tables.

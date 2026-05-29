# Dirty Page Buffer Merge Guard Leaf Shapes

## Problem

The full future-current direct-write slice intentionally left partial
future-current index leaves on fallback replay because a broader direct-write
experiment regressed the prepared-insert step to `94.432 us/op`. The retained
policy still reports a large fallback class: `113,367` dirty `index-leaf`
`future-current-header-partial-leaf` rows in the latest VPS profile.

The current guard outcome table explains why merge entries direct-write or
fall back, but it does not say whether the partial fallback leaves are nearly
full, sparse, or distributed across free-slot ranges. Without that shape
evidence, the next behavior slice would have to guess whether any subset of
partial future-current leaves can be published safely and profitably.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice is first-party MyLite storage and benchmark instrumentation only.
  It does not change upstream MariaDB handler code.
- `merge_dirty_page_buffer()` classifies every child dirty-buffer entry with
  `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()` before
  direct-write or fallback replay.
- `record_dirty_page_buffer_merge_direct_write_guard_outcome()` already records
  guard outcomes by page family and checksum-dirty state in test-hook builds.
- The existing helper `dirty_page_buffer_leaf_fill_band()` classifies valid
  index leaves as `empty`, `1-24%`, `25-49%`, `50-74%`, `75-99%`, or `full`.
- The existing helper `dirty_page_buffer_leaf_free_slot_band()` classifies
  valid index leaves by free slots: `0`, `1`, `2-3`, `4-7`, `8-15`, and
  `16+`.

## Design

Add test-hook-only guard outcome cross-tabs for index-leaf shape:

- guard outcome by leaf fill band;
- guard outcome by leaf free-slot band.

The counters are recorded at the existing guard outcome accounting point, so
each child dirty-buffer merge entry contributes at most one guard outcome row
and, when the page is an index leaf, one fill-band row and one free-slot row.
The benchmark prints nonzero leaf-shape rows immediately after the guard
outcome table in the prepared-insert component profile.

The slice does not change the direct-write guard, dirty-buffer merge behavior,
rollback rules, append-buffer publication, page layout, or durable file
format.

## Compatibility Impact

No SQL syntax, public C API, handler API, storage-engine routing, metadata, or
file-format behavior changes. `ENGINE=InnoDB` and other routed storage-engine
semantics continue through the same MyLite storage path.

## Single-File And Lifecycle Impact

No files are introduced and no publication order changes. Durable state remains
in the primary `.mylite` file plus the existing MyLite-owned journal lifecycle.
The new state is thread-local test-hook evidence reset by
`mylite_storage_test_reset_prepared_insert_profile_counts()`.

## Public API And Binary Impact

No public API changes. Test-hook builds expose two internal profiling accessors
for benchmark and self-test use. Production builds do not gain new counters or
dependencies.

## Tests And Verification

- Extend the focused future-page relation self-test to assert that:
  - a full future-current direct-write leaf records `full` fill band and `0`
    free slots for the `future-current-header-direct-write` guard outcome;
  - append-buffer fallback leaves record `empty` fill band and `16+` free
    slots for `future-append-buffer`;
  - the partial future-current fallback leaf records `empty` fill band and
    `16+` free slots for `future-current-header-partial-leaf`;
  - the past-current fallback leaf records `empty` fill band and `16+` free
    slots for `future-page`.
- Add prepared-insert benchmark tables for guard outcome leaf fill bands and
  guard outcome leaf free-slot bands.
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

- Guard outcome leaf fill-band and free-slot counters are reset with the
  prepared-insert profile counters.
- Focused storage self-tests prove the expected guard outcome and leaf-shape
  buckets for direct-write, append-buffer fallback, partial future-current
  fallback, and past-current fallback rows.
- The prepared-insert benchmark reports nonzero guard leaf-shape rows for the
  current workload.
- No storage behavior, file lifecycle, public API, or compatibility claim
  changes.

## Verification Evidence

VPS prepared-insert evidence after implementation:

- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  reported `76.710 us/op` for the prepared insert step.
- `3,330` `future-current-header-direct-write` `index-leaf` rows were all
  `full` with `0` free slots.
- `113,367` `future-current-header-partial-leaf` rows split by fill band as
  `1,446` in `1-24%`, `1,501` in `25-49%`, `20,206` in `50-74%`, and
  `90,214` in `75-99%`.
- The same partial fallback rows split by free-slot band as `3,438` with `1`
  free slot, `5,753` with `2-3`, `8,913` with `4-7`, `13,384` with `8-15`,
  and `81,879` with `16+`.
- `448` `parent-resident` guard rows were `full` leaves with `0` free slots.

## Risks

- These counters are evidence, not a proof that any partial-leaf direct-write
  policy is profitable. Future behavior slices must still validate rollback,
  append-buffer interaction, and prepared-insert timing before widening direct
  writes.
- The fill/free-slot bands reuse existing leaf validators. Invalid leaf
  metadata is counted under the existing `invalid` buckets, which is useful for
  diagnostics but should not be interpreted as a safe publication class.

# Dirty-Page Buffer Merge Fallback Parent Rank Counters

## Problem

The current future-current merge policy direct-writes full, near-full, and
`16-31` free-slot index leaves. The remaining `32+` free-slot
`future-current-header-partial-leaf` fallback rows still show substantial
parent dirty-buffer coalescing, and a bounded `32-63` direct-write experiment
regressed the prepared insert step. Raw free-slot detail is therefore not
enough to decide which fallback leaves are cold enough to publish directly.

The previous replacement counters connect a fallback leaf to later replacement
events and final flush state, but they do not expose whether the admitted page
was below the parent dirty buffer's current leaf tail or at/above it. That rank
is available at merge time and may separate older non-tail pages from new tail
growth pages.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice only changes first-party MyLite storage test hooks and benchmark
  reporting in `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/tests/storage_test.c`, and
  `tools/mylite_perf_baseline.c`; no upstream MariaDB handler or SQL source is
  changed.
- `merge_dirty_page_buffer()` computes a direct-write guard outcome before
  either direct-writing a child dirty-page entry or replaying it into the parent
  dirty-page buffer.
- `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()` leaves
  `32+` future-current index leaves on fallback as
  `future-current-header-partial-leaf`.
- `store_dirty_page_in_buffer()` tags entries admitted through merge fallback
  with test-hook origin metadata, tracks replacement counts, and records leaf
  replacement change classes.
- Existing flush leaf page-id rank counters classify the dirty buffer at flush
  time. This slice needs a predictor captured at merge admission, before the
  entry has had a chance to coalesce.

## Design

Add a test-hook-only parent leaf page-id rank to merge fallback leaf origin
metadata. At merge fallback admission, inspect the parent dirty-page buffer's
current index-leaf pages and classify the incoming leaf as:

- `no-parent-leaf` when the parent dirty buffer has no index leaves;
- `below-parent-max-leaf-page-id` when the incoming page id is below the
  current maximum parent buffered leaf page id;
- `at-or-above-parent-max-leaf-page-id` when the incoming page id is at or
  above the current maximum parent buffered leaf page id.

Use that rank to publish three counter families:

- merge fallback leaf admissions by parent rank, guard outcome, and admitted
  free-slot detail;
- merge fallback leaf replacement events by parent rank, guard outcome,
  admitted free-slot detail, and leaf change class;
- merge fallback leaf flush replacement states by flush source, parent rank,
  guard outcome, admitted free-slot detail, and final replacement state.

The counters are observational. They do not change direct-write eligibility,
dirty-buffer replacement behavior, page layout, rollback, journaling, or file
format. Non-test-hook builds do not carry the new fields or counters.

## Compatibility Impact

No SQL syntax, public C API, handler API, metadata, storage-engine routing, or
file-format behavior changes. `ENGINE=InnoDB` continues to resolve through the
MyLite storage engine.

## Single-File And Lifecycle Impact

No files or durable state are introduced. The new rank lives only in
test-hook builds and only inside in-memory dirty-page buffer entries.

## Public API And Binary Impact

No public API changes and no dependencies. Test-hook builds gain extra counter
storage and exported test-hook accessors. Non-test-hook builds do not carry the
new field or counters.

## Tests And Verification

- Add a focused storage self-test that admits a
  `future-current-header-partial-leaf` fallback entry below the parent dirty
  buffer leaf tail, replaces it, flushes it, and asserts the new admission,
  replacement, and flush-state counters.
- Extend prepared-insert component benchmark output with nonzero tables for the
  new parent-rank counter families.
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

- Test-hook builds can attribute merge fallback leaf admissions to the parent
  dirty-buffer leaf page-id rank available at merge time.
- Test-hook builds can correlate that parent rank with later replacement events
  and final flush replacement state.
- Prepared-insert benchmark output can show whether `32+`
  `future-current-header-partial-leaf` fallback coalescing is concentrated
  below the parent leaf tail or at the current/new tail.
- No committed direct-write behavior changes.

## Verification Evidence

VPS prepared-insert component evidence after implementation:

- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  reported `74.553 us/op` for the prepared insert step.
- The run retained the committed behavior counters: `53,136` dirty
  `index-leaf` merge direct writes, `34,484` dirty `index-leaf` pressure
  admissions, `115,619` branch entry-count fast replacements, `13,922` branch
  entry-count fence fast replacements, and `33,851` leaf growth fast
  replacements.
- All `122,388` dirty `index-leaf` future-page relation rows remained
  `within-current-header` with append relation `none`.
- `future-current-header-partial-leaf` fallback admissions were almost all
  below the parent dirty buffer's current max leaf page id: `18,348` of
  `18,349` `32-63` rows, `14,122` of `14,152` `64-127` rows, and `1,725` of
  `1,983` `128+` rows.
- The small at-or-above parent max group was replacement-heavy for broad
  leaves: the `128+` group admitted only `258` rows but recorded `3,986`
  append and `9,232` insert replacement events.
- Below-parent-max fallback rows also coalesced: `32-63` rows recorded `94`
  append and `5,226` insert replacement events; `64-127` rows recorded `79`
  append and `5,569` insert replacement events; `128+` rows recorded `288`
  append and `3,792` insert replacement events.

## Risks

- The parent rank is a heuristic based on page id order in the parent dirty
  buffer, not a full B-tree semantic position.
- The counters add test-hook bookkeeping around a hot test path, so benchmark
  values should be compared only against other test-hook benchmark runs.
- Replacement evidence does not by itself prove a new publication policy; it
  only identifies candidate classes for a later bounded behavior slice.

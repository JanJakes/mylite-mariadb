# Dirty-Page Buffer Merge Fallback Tail Distance Counters

## Problem

Parent-rank counters show almost all remaining `32+`
`future-current-header-partial-leaf` fallback admissions are below the parent
dirty buffer's current max leaf page id. That rank is too coarse for a
behavior change: the same below-max `32-63` group records thousands of
replacement events, while many entries still flush without replacement.

The next predictor should preserve the admission-time shape that is already
available during merge while breaking the broad below-max class into page-id
distance bands from the parent dirty-buffer leaf tail.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice only changes first-party MyLite storage test hooks and benchmark
  reporting in `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/tests/storage_test.c`, and
  `tools/mylite_perf_baseline.c`; no upstream MariaDB handler or SQL source is
  changed.
- `merge_dirty_page_buffer()` already computes test-hook merge fallback origin
  metadata before replaying the child entry into the parent dirty-page buffer.
- `dirty_page_buffer_merge_fallback_parent_leaf_page_id_rank_for_entry()`
  scans the parent dirty-page buffer for the current max buffered leaf page id.
  The same source information can classify the incoming page-id distance from
  that max before pressure flush or replacement changes the buffer.
- The prepared-insert smoke profile after the parent-rank slice reports
  `18,348` of `18,349` `32-63` future-current fallback admissions below the
  parent max leaf, so rank alone cannot justify a behavior change.
- The implemented tail-distance profile reports remaining
  `future-current-header-partial-leaf` fallback admissions concentrated far
  behind the parent dirty-buffer leaf tail: `12,036` in the `32-127` band and
  `20,215` in the `128+` band. The small `at-or-above` group admits only
  `289` rows but records `13,469` replacement events, while the `128+`
  below-tail group records `10,793` replacement events. Tail distance is a
  useful discriminator, but still not a standalone publication proof.

## Design

Add a test-hook-only parent leaf tail-distance band to merge fallback leaf
origin metadata. At fallback admission, classify the incoming leaf as:

- `no-parent-leaf`;
- `at-or-above-parent-max-leaf-page-id`;
- `below-parent-max-by-1`;
- `below-parent-max-by-2-7`;
- `below-parent-max-by-8-31`;
- `below-parent-max-by-32-127`;
- `below-parent-max-by-128+`.

Use that band to publish three counter families:

- merge fallback leaf admissions by tail-distance band, guard outcome, and
  admitted free-slot detail;
- merge fallback leaf replacement events by tail-distance band, guard outcome,
  admitted free-slot detail, and leaf change class;
- merge fallback leaf flush replacement states by flush source, tail-distance
  band, guard outcome, admitted free-slot detail, and final replacement state.

The counters are observational. They do not change direct-write eligibility,
dirty-buffer replacement behavior, page layout, rollback, journaling, or file
format. Non-test-hook builds do not carry the new field or counters.

The tail-distance counter tensor is allocated lazily from a thread-local
pointer instead of as static thread-local arrays. Keeping the added
high-dimensional storage off static TLS preserves embedded MariaDB startup
headroom; an earlier static-TLS version caused the storage-smoke embedded
test to abort before creating its timer thread.

## Compatibility Impact

No SQL syntax, public C API, handler API, metadata, storage-engine routing, or
file-format behavior changes. `ENGINE=InnoDB` continues to resolve through the
MyLite storage engine.

## Single-File And Lifecycle Impact

No files or durable state are introduced. The new band lives only in
test-hook builds and only inside in-memory dirty-page buffer entries.

## Public API And Binary Impact

No public API changes and no dependencies. Test-hook builds gain extra counter
storage and exported test-hook accessors. The large tail-distance counter
families are lazy heap storage behind one thread-local pointer. Non-test-hook
builds do not carry the new field or counters.

## Tests And Verification

- Extend the focused storage self-test that admits a
  `future-current-header-partial-leaf` fallback entry below the parent dirty
  buffer leaf tail by a known page-id distance, replaces it, flushes it, and
  asserts the new admission, replacement, and flush-state counters.
- Extend prepared-insert component benchmark output with nonzero tables for the
  new tail-distance counter families.
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

- Test-hook builds can attribute merge fallback leaf admissions to the
  admission-time page-id distance from the parent dirty-buffer max leaf.
- Test-hook builds can correlate that distance band with later replacement
  events and final flush replacement state.
- Prepared-insert benchmark output can show whether hot below-max coalescing is
  concentrated near the parent leaf tail or spread across older leaf pages.
- No committed direct-write behavior changes.

## Risks

- Page-id distance is a heuristic for allocation/dirty-buffer locality, not a
  full B-tree semantic distance.
- The counters add test-hook bookkeeping around a hot test path, so benchmark
  values should be compared only against other test-hook benchmark runs.
- Replacement evidence does not by itself prove a new publication policy; it
  only identifies candidate classes for a later bounded behavior slice.

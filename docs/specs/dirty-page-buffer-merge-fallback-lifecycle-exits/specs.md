# Dirty-Page Buffer Merge Fallback Lifecycle Exits

## Problem

The replaced-broad-victim direct-write path now has complete lifecycle exit
accounting, but the fallback-admitted merge leaves still only report
admissions, replacements, pressure victims, and flush states. The remaining
rejected below-tail candidates are the next measured policy surface, so their
non-flush exits should be visible before another direct-write behavior change.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage test hooks, storage self-tests,
  benchmark reporting, and documentation only.
- `packages/mylite-storage/src/storage.c`:
  - `tag_dirty_page_buffer_entry_merge_fallback_origin()` tags fallback-admitted
    merge leaves with guard outcome, parent leaf page-id rank, parent leaf tail
    distance, and admitted leaf free-slot detail.
  - `record_dirty_page_buffer_merge_fallback_leaf_replacement()` records later
    in-buffer rewrites of tagged fallback leaves.
  - `record_dirty_page_buffer_merge_fallback_leaf_flush_replacement_state()`
    records tagged fallback leaves that leave through dirty-buffer flush paths.
  - `discard_dirty_page_buffer_entry()` removes one dirty-buffer entry without
    flushing it.
  - `clear_dirty_page_buffer()` frees remaining dirty-buffer entries during
    rollback, cleanup, and statement teardown.
- `tools/mylite_perf_baseline.c` already prints fallback tail-distance
  admissions, replacements, flush states, and rejected below-tail summaries.

## Design

Add heap-backed test-hook counters for fallback leaf non-flush exits:

- tail-distance discard counts, recorded before
  `discard_dirty_page_buffer_entry()` unlinks a tagged fallback leaf;
- tail-distance clear counts, recorded before `clear_dirty_page_buffer()` frees
  tagged fallback leaves.

The counters are keyed by existing fallback-origin dimensions:

- parent leaf tail distance;
- merge direct-write guard outcome;
- admitted leaf free-slot detail.

The prepared-insert benchmark prints two full tail-distance tables and adds
compact rejected below-tail candidate `discards` and `clears` summary rows.
Those summary rows reuse the existing rejected below-tail predicate:
`future-current-header-partial-leaf` fallback leaves with `32-127` free slots
and a `32-127` page distance below the parent dirty-buffer leaf tail.

## Compatibility Impact

No SQL behavior, public MyLite C API, handler API, storage-engine routing, or
file-format behavior changes. `ENGINE=InnoDB` continues to route through
MyLite. All new state and accessors are test-hook-only.

## Single-File And Lifecycle Impact

No durable state, sidecars, journal layout, rollback layout, or embedded
lifecycle changes. The new counters observe existing dirty-buffer exit paths
without changing admission, flushing, discard, clear, rollback, or recovery
behavior.

## Public API And File Format Impact

No public API or on-disk format changes. New symbols are internal test-hook
accessors used by the storage self-test and performance baseline tool.

## Storage-Engine Routing Impact

No routing change. Supported MySQL/MariaDB storage-engine names, including
`ENGINE=InnoDB`, continue to resolve through the MyLite storage layer.

## Binary-Size Impact

No new dependencies. Test-hook builds gain two small heap-backed tail-distance
counter tensors and benchmark output loops.

## Tests And Verification

- Add a focused storage self-test that tags synthetic fallback-origin dirty
  leaves and asserts:
  - `discard_dirty_page_buffer_entry()` records one tail-distance discard and no
    clear;
  - `clear_dirty_page_buffer()` records one tail-distance clear and no discard;
  - the compact rejected below-tail discard/clear summary accessors count the
    same tagged entries.
- Extend the prepared-insert benchmark with fallback tail-distance discard and
  clear tables plus rejected below-tail summary rows.
- Current storage-smoke evidence for
  `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  - prepared insert step: `75.224 us/op`;
  - direct dirty `index-leaf` merge writes: `55,902`;
  - dirty `index-leaf` pressure admissions: `31,979`;
  - rejected below-tail candidate admissions: `9,619`;
  - rejected below-tail candidate buffer-limit flushes: `9,610`;
  - rejected below-tail candidate discards: `9`;
  - rejected below-tail candidate clears: `0`;
  - rejected below-tail admissions reconcile exactly as `9,610` flushes plus
    `9` discards and `0` clears.
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

- Tagged fallback leaves that exit by discard or clear are counted by
  tail-distance, guard outcome, and admitted free-slot detail.
- The benchmark reports compact rejected below-tail candidate non-flush exits.
- Existing merge direct-write, fallback replay, flush, rollback, and
  pressure-selection behavior is unchanged.

## Risks

- Discard and clear counters are exit counters, not admission counters. A page
  that flushes and then has its dirty-buffer slot reused should continue to be
  represented by the existing flush-state table, not the new non-flush tables.

# Dirty-Page Buffer Merge Fallback Pressure Victim Free-Slot Replacement Matrix

## Problem

The rejected below-tail admitted/victim free-slot matrix shows that broad
incoming fallback leaves mostly evict broad dirty leaves. The admitted/victim
replacement-state matrix shows that most of those victims are never replaced
before eviction. Those two summaries are still marginal: they do not show
whether the already-replaced victims are concentrated in the same broad
free-slot bands, or whether a stricter predicate can isolate a small group of
useful victims before any direct-write behavior change.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage test hooks and benchmark
  reporting only.
- `packages/mylite-storage/src/storage.c`:
  - `record_dirty_page_buffer_merge_fallback_leaf_pressure_victim()` already
    has the incoming fallback leaf page, selected victim entry, victim free-slot
    detail, and victim replacement state at the same pressure-selection point.
  - `mylite_storage_test_dirty_page_buffer_merge_rejected_below_tail_direct_write_candidate_pressure_victim_leaf_free_slot_detail_matrix_count()`
    summarizes admitted free slots against victim free slots.
  - `mylite_storage_test_dirty_page_buffer_merge_rejected_below_tail_direct_write_candidate_pressure_victim_replacement_state_matrix_count()`
    summarizes admitted free slots against victim replacement state.
  - `mylite_storage_test_dirty_page_buffer_merge_fallback_tracks_parent_leaf_page_id_rank()`
    is the focused storage self-test for the synthetic rejected below-tail
    candidate and pressure victim.
- `tools/mylite_perf_baseline.c` prints the prepared-insert dirty-page buffer
  merge rejected-candidate pressure-victim tables.

## Design

Add one heap-backed test-hook tensor that records:

- parent leaf tail-distance band;
- merge direct-write guard outcome;
- admitted incoming leaf free-slot detail;
- victim leaf free-slot detail;
- victim leaf replacement state.

Expose a rejected below-tail summary accessor that returns one admitted/victim
free-slot/replacement-state matrix cell for the existing rejected predicate:
`future-current-header-partial-leaf`, admitted `32-63` or `64-127` free slots,
and `32-127` page distance below the parent dirty-buffer leaf tail.

The prepared-insert benchmark prints nonzero rows with:

- admitted rejected-candidate leaf free slots;
- victim leaf free slots;
- victim leaf replacement state;
- victim page count.

Existing marginal pressure-victim summaries remain unchanged.

## Compatibility Impact

No SQL behavior, public MyLite C API, handler API, storage-engine routing, or
file-format behavior changes. `ENGINE=InnoDB` continues to route through
MyLite. The new function is a test-hook-only benchmark helper.

## Single-File And Lifecycle Impact

No durable state, sidecars, journal layout, rollback layout, or embedded
lifecycle changes. The new tensor is process-local test-hook state and is reset
with the existing prepared-insert profile reset path.

## Public API And File Format Impact

No public API or on-disk format changes. The new symbol is an internal
test-hook accessor used by storage self-tests and the local benchmark tool.

## Storage-Engine Routing Impact

No routing change. Supported MySQL/MariaDB storage-engine names, including
`ENGINE=InnoDB`, continue to resolve through the MyLite storage layer.

## Binary-Size Impact

No new dependencies. The slice adds one heap-backed test-hook tensor, one
summary accessor, focused storage assertions, and benchmark output.

## Tests And Verification

- Extend the focused dirty-page buffer merge fallback parent-rank/tail-distance
  storage self-test so the synthetic rejected below-tail candidate reports one
  matrix cell for admitted `32-63` free slots displacing a never-replaced
  `128+` free-slot victim.
- Extend the prepared-insert benchmark output with the rejected below-tail
  admitted-free-slot/victim-free-slot/victim-replacement-state matrix.
- Implementation evidence on `custom-storage`:
  - dev `mylite-storage` CTest passed in `297.76 sec`;
  - embedded static smoke build completed with `libmariadbd.a` at
    `33,974,138` bytes;
  - storage-smoke CTests passed, including `mylite-storage` in `301.55 sec`
    and `libmylite.embedded-storage-engine` in `14.27 sec`;
  - prepared-insert benchmark reported a `75.797 us/op` prepared insert step,
    `53,136` dirty leaf direct merge writes, `34,484` dirty leaf pressure
    admissions, and `11,971` rejected below-tail candidate admissions;
  - the triple matrix reported broad `32-127` victims that were already replaced
    for `1,194` rejected-candidate admissions;
  - broad `32-127` victims that were never replaced accounted for `10,236`
    rejected-candidate admissions;
  - victims below `32` free slots accounted for only `87` admissions, split
    across replacement states.
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

- The benchmark reports a rejected below-tail admitted-free-slot/victim-free-slot
  and victim-replacement-state matrix.
- The matrix is recorded at the existing pressure-victim selection point and
  does not change merge direct-write, fallback replay, flush, or
  pressure-selection behavior.
- Focused storage tests and prepared-insert benchmark output cover the new
  matrix accessor.

## Risks

- The matrix still describes failed direct-write candidates; it can narrow the
  next behavior predicate, but benchmark validation remains required before any
  direct-write or pressure-selection change is adopted.

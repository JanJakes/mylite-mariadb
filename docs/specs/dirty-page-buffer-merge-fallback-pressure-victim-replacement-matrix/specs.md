# Dirty-Page Buffer Merge Fallback Pressure Victim Replacement Matrix

## Problem

The rejected below-tail free-slot matrix shows that broad incoming fallback
leaves mostly evict broad non-tail dirty leaves. The existing marginal victim
replacement-state summary shows most victims are never replaced before
eviction, but it does not show whether the `32-63` and `64-127` incoming
candidate groups evict different victim replacement-state mixes.

Before attempting another conditional direct-write experiment, the prepared
insert profile needs to show whether the promising admitted/victim free-slot
pairs are displacing cold victims or leaves that already absorbed rewrites in
the parent dirty buffer.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage test hooks and benchmark
  reporting only.
- The merge fallback pressure-victim counters already store incoming fallback
  free-slot detail and victim leaf replacement state in the same heap-backed
  tensor.
- `packages/mylite-storage/src/storage.c`:
  - `mylite_storage_test_dirty_page_buffer_merge_fallback_leaf_tail_distance_pressure_victim_replacement_state_count()`
    reads the existing distance/outcome/admitted-free-slot/victim-replacement
    tensor.
  - `is_rejected_below_tail_direct_write_candidate_free_slot_detail()` defines
    the admitted free-slot bands used by the rejected below-tail predicate.
  - `mylite_storage_test_dirty_page_buffer_merge_fallback_tracks_parent_leaf_page_id_rank()`
    is the focused storage self-test for the synthetic rejected candidate and
    pressure victim.
- `tools/mylite_perf_baseline.c` prints the prepared-insert dirty-page buffer
  merge rejected-candidate summaries that guide this slice.
- The rejected below-tail predicate is already defined as
  `future-current-header-partial-leaf` admissions with `32-63` or `64-127`
  incoming free slots and a `32-127` page distance below the parent dirty-buffer
  leaf tail.

## Design

Add a test-hook summary accessor that returns rejected below-tail
pressure-victim counts for one admitted leaf free-slot detail and one victim
leaf replacement state. The accessor reads the existing heap-backed pressure
victim tensor; it does not add new counters.

The prepared-insert benchmark prints a compact matrix:

- admitted rejected-candidate leaf free slots;
- victim leaf replacement state;
- victim page count.

Existing marginal rejected-candidate victim summaries and the admitted/victim
free-slot matrix remain unchanged.

## Compatibility Impact

No SQL behavior, public MyLite C API, handler API, storage-engine routing, or
file-format behavior changes. `ENGINE=InnoDB` continues to route through
MyLite. The new function is a test-hook-only benchmark helper.

## Single-File And Lifecycle Impact

No durable state, sidecars, journal layout, rollback layout, or embedded
lifecycle changes. The summary reads process-local test-hook counters and is
reset with the existing prepared-insert profile reset path.

## Public API And File Format Impact

No public API or on-disk format changes. The new symbol is an internal
test-hook accessor used by storage self-tests and the local benchmark tool.

## Storage-Engine Routing Impact

No routing change. Supported MySQL/MariaDB storage-engine names, including
`ENGINE=InnoDB`, continue to resolve through the MyLite storage layer.

## Binary-Size Impact

No new dependencies and no new counter storage. The slice adds one test-hook
summary accessor and benchmark output.

## Tests And Verification

- Extend the focused dirty-page buffer merge fallback parent-rank/tail-distance
  storage self-test so the synthetic rejected below-tail candidate reports one
  matrix cell for admitted `32-63` free slots displacing a never-replaced
  victim.
- Extend the prepared-insert benchmark output with the rejected below-tail
  admitted-free-slot/victim-replacement-state matrix.
- Implementation evidence on `custom-storage`:
  - dev `mylite-storage` CTest passed in `303.66 sec`;
  - embedded static smoke build completed with `libmariadbd.a` at
    `33,974,138` bytes;
  - storage-smoke CTests passed, including `mylite-storage` in `323.30 sec`
    and `libmylite.embedded-storage-engine` in `15.41 sec`;
  - prepared-insert benchmark structural counters stayed stable under unrelated
    concurrent host load, with a `114.751 us/op` prepared insert step sample,
    `53,136` dirty leaf direct merge writes, `34,484` dirty leaf pressure
    admissions, and `11,971` rejected below-tail candidate admissions;
  - the replacement-state matrix reported `32-63` incoming candidates evicting
    `4,862` never-replaced victims, `409` replaced-once victims, and `356`
    replaced-multiple victims;
  - the matrix reported `64-127` incoming candidates evicting `5,775`
    never-replaced victims, `393` replaced-once victims, and `176`
    replaced-multiple victims.
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

- The benchmark reports a rejected below-tail admitted-free-slot/victim
  replacement-state matrix.
- The matrix is computed from existing pressure-victim replacement-state
  counters.
- Existing merge direct-write, fallback replay, flush, and pressure-selection
  behavior is unchanged.
- Focused storage tests and prepared-insert benchmark output cover the new
  summary accessor.

## Risks

- Replacement state is a dirty-buffer slot history, not a complete predictor of
  future coalescing. It should guide candidate selection alongside free-slot
  and benchmark evidence.

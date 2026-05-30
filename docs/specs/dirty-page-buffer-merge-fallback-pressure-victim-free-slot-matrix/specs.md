# Dirty-Page Buffer Merge Fallback Pressure Victim Free-Slot Matrix

## Problem

Rejected below-tail fallback candidates and the dirty index-leaf victims they
evict are both broad non-tail partial leaves in the current prepared-insert
profile. The marginal victim free-slot totals show that most victims have
`32-127` free slots, but they do not show whether `32-63` incoming candidates
evict similarly full victims or whether `64-127` incoming candidates evict
more valuable narrower leaves.

The next policy decision needs a pairwise view of the incoming candidate shape
against the selected victim shape before any direct-write or pressure-selector
change.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage test hooks and benchmark
  reporting only.
- The merge fallback pressure-victim counters already store incoming fallback
  free-slot detail and victim leaf free-slot detail in the same heap-backed
  tensor.
- The rejected below-tail predicate is already defined as
  `future-current-header-partial-leaf` admissions with `32-63` or `64-127`
  incoming free slots and a `32-127` page distance below the parent dirty-buffer
  leaf tail.

## Design

Add a test-hook summary accessor that returns rejected below-tail
pressure-victim counts for one admitted leaf free-slot detail and one victim
leaf free-slot detail. The accessor reads the existing heap-backed pressure
victim shape tensor; it does not add new counters.

The prepared-insert benchmark prints a compact matrix:

- admitted rejected-candidate leaf free slots;
- victim leaf free slots;
- victim page count.

Existing marginal rejected-candidate victim family, replacement-state, fill,
free-slot, and page-id-rank summaries remain unchanged.

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
  matrix cell for admitted `32-63` free slots displacing a victim with `128+`
  free slots.
- Extend the prepared-insert benchmark output with the rejected below-tail
  admitted/victim free-slot matrix.
- Implementation evidence on `custom-storage`:
  - dev `mylite-storage` CTest passed in `301.07 sec`;
  - embedded static smoke build completed with `libmariadbd.a` at
    `33,974,138` bytes;
  - storage-smoke CTests passed, including `mylite-storage` in `402.47 sec`
    and `libmylite.embedded-storage-engine` in `14.89 sec`;
  - prepared-insert benchmark reported a `71.475 us/op` prepared insert step,
    `53,136` dirty leaf direct merge writes, and `34,484` dirty leaf pressure
    admissions;
  - rejected below-tail candidate admissions remained `11,971`, with all
    `11,971` pressure victims reported as checksum-dirty `index-leaf` pages;
  - the rejected-candidate matrix reported `32-63` incoming candidates evicting
    `3,704` `32-63` victims, `1,698` `64-127` victims, and `169` `128+`
    victims;
  - the matrix reported `64-127` incoming candidates evicting `2,554` `32-63`
    victims, `3,474` `64-127` victims, and `285` `128+` victims;
  - only `87` rejected-candidate pressure victims had fewer than `32` free
    slots.
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

- The benchmark reports a rejected below-tail admitted/victim free-slot matrix.
- The matrix is computed from existing pressure-victim shape counters.
- Existing merge direct-write, fallback replay, flush, and pressure-selection
  behavior is unchanged.
- Focused storage tests and prepared-insert benchmark output cover the new
  summary accessor.

## Risks

- The matrix is still a summary of the failed direct-write predicate, not a
  complete policy proof. It should guide the next candidate selection but not
  replace benchmark validation for behavior changes.

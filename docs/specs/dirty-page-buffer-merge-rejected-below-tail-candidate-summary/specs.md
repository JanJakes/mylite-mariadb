# Dirty-Page Buffer Merge Rejected Below-Tail Candidate Summary

## Problem

The prepared-insert benchmark already reports fallback tail-distance,
free-slot, replacement, and flush-state cross-tabs for future-current partial
leaf merge rows. The useful rejected direct-write predicate is hard to read
from those wide tables: future-current partial leaves with `32-127` free slots
whose page id is `32-127` pages below the parent dirty-buffer leaf tail.

That predicate was tested as a behavior change and rejected because it
regressed the prepared insert step. The next evidence slice should make that
same predicate visible as a concise benchmark summary without changing merge
publication behavior.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage test hooks and benchmark
  reporting only.
- `merge_dirty_page_buffer()` already tags fallback leaf admissions with the
  direct-write guard outcome, admitted free-slot detail, and parent leaf
  tail-distance band.
- Heap-backed guard/fallback counter storage now lets test-hook summary
  accessors depend on those high-dimensional counters without increasing
  static TLS.

## Design

Add test-hook summary accessors for the rejected below-tail candidate:

- guard outcome is `future-current-header-partial-leaf`;
- parent leaf tail distance is `below-parent-max-by-32-127`;
- admitted leaf free-slot detail is either `32-63` or `64-127`.

The accessors sum existing heap-backed tail-distance admission, replacement,
and flush replacement-state counters. No new storage behavior, file format,
or durable state is introduced. The prepared-insert benchmark prints a compact
candidate summary table using those accessors.

## Compatibility Impact

No SQL behavior, public MyLite C API, handler API, storage-engine routing, or
file-format behavior changes. `ENGINE=InnoDB` continues to route through
MyLite. The new functions are test-hook-only benchmark helpers.

## Single-File And Lifecycle Impact

No files, sidecars, or durable lifecycle changes. The summary reads
process-local test-hook counters only.

## Tests And Verification

- Extend the focused storage self-test so a future-current partial leaf in the
  rejected candidate band reports one admission, one append replacement, and
  one flush replacement-state count through the new summary accessors.
- Verify the prepared-insert benchmark prints the candidate summary.
- Implementation evidence on `custom-storage`:
  - dev `mylite-storage` CTest passed in `334.00 sec`;
  - embedded static smoke build completed with `libmariadbd.a` at
    `33,974,138` bytes;
  - storage-smoke CTests passed, including `mylite-storage` in `295.93 sec`
    and `libmylite.embedded-storage-engine` in `14.12 sec`;
  - prepared-insert benchmark printed the rejected below-tail candidate
    summary with `11,971` admissions, `24` append replacements, `2,191`
    insert replacements, and buffer-limit flush states of `11,538` never
    replaced, `185` replaced once, and `238` replaced multiple times;
  - the same benchmark run reported `81.177 us/op` for the prepared insert
    step component, `53,136` dirty leaf direct merge writes, and `34,484`
    dirty leaf pressure admissions.
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

- The rejected below-tail candidate summary is computed from existing
  heap-backed tail-distance counters.
- Focused storage tests cover admission, replacement, and flush-state summary
  accessors.
- Benchmark output includes a concise candidate summary.
- No direct-write policy, dirty-buffer replacement, rollback, journaling, or
  file-format behavior changes.

## Risks

- The summary can hide detail that remains visible in the full tail-distance
  tables. Keep the full tables unchanged and use the summary only as a
  convenience view.

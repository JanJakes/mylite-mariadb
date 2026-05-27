# Packed Index Tail Scan Breakdown

## Problem

After the packed index tail memo advance, the VPS prepared-insert benchmark
still reports `packed index tail-append scan pages = 17`. The aggregate counter
proves the scan volume is small, but it no longer identifies whether the
remaining work is compatible row pages, other-shape index pages, same-shape
barriers, missing buffered pages, or invalid tail pages.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/mylite-storage/src/storage.c::cached_packed_index_entry_page_allows_tail_append()`
  owns the tail validation loop and already increments the aggregate test-hook
  scan-page counter.
- `tools/mylite_perf_baseline.c::print_prepared_insert_storage_counters()`
  prints the prepared-insert branch and packed-tail counters used for recent
  performance slices.
- The scan loop already classifies enough state to split the counter without
  changing storage behavior: buffered page missing, row page, structurally
  valid index-entry page, same-shape index-entry barrier, and other page types.

## Design

- Keep the existing aggregate packed-tail scan-page counter.
- Add test-hook-only counters for:
  - scanned row pages;
  - scanned other-shape index-entry pages;
  - scanned same-shape index-entry barrier pages;
  - scanned missing buffered pages;
  - scanned invalid or incompatible tail pages.
- Reset the detailed counters with the existing packed-tail reset hook.
- Print the detailed counters in the prepared-insert component benchmark.
- Extend the existing packed-tail memoization test hook to assert the row,
  other-index, and same-shape breakdown in its synthetic tail scans.

## Compatibility Impact

No SQL, public API, file format, storage-engine routing, or single-file
lifecycle behavior changes. The slice only adds `MYLITE_STORAGE_TEST_HOOKS`
instrumentation and benchmark output.

## Scope And Implications

- Affected subsystem: storage-smoke test hooks and local performance harness.
- DDL metadata routing, MariaDB handler behavior, write path behavior, durable
  page formats, and public `libmylite` API: unchanged.
- Dependency, license, binary-size impact: none for production builds; test-hook
  counters only.

## Test And Verification Plan

- Extend packed-tail test-hook coverage for row, other-index, and same-shape
  scan categories.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Prepared-insert benchmark output explains the aggregate packed-tail scan-page
  counter by page kind or blocker reason.
- Existing aggregate counter remains available for continuity with prior
  performance slices.
- Existing storage and routed storage-engine tests pass.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed; the remaining `17` packed index tail-append scan pages were all
  `missing-page blockers`, with row-page, other-index, same-index, and invalid
  blockers at `0`.

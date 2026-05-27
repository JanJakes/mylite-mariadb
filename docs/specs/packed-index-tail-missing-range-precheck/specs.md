# Packed Index Tail Missing Range Precheck

## Problem

The packed tail scan breakdown shows the remaining `17` prepared-insert
packed-tail scan pages are all missing-page blockers. A missing page means the
validation range cannot be covered by the active append buffer, so the cached
packed index-entry page cannot be safely reused. The current loop still probes
the first missing page before returning that failure.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/mylite-storage/src/storage.c::cached_packed_index_entry_page_allows_tail_append()`
  already rejects the cache when `buffered_append_page_ref_in_statement()`
  cannot find a tail page.
- The active append buffer exposes a contiguous range through
  `mylite_storage_statement::append_pages.first_page_id` and `page_count`.
- If the unchecked tail range reaches outside that contiguous buffer range,
  the validator cannot prove there are no same-shape or incompatible pages;
  rejecting before the scan loop preserves the same append-safety behavior.

## Design

- Before the scan loop, compare the unchecked tail range with the active append
  buffer range.
- If any page in `[first_tail_page_id, header->page_count)` is outside the
  append buffer, reject the cache immediately.
- Keep the detailed missing-page blocker counter so the benchmark still reports
  why the cache was rejected, but do not increment the aggregate scanned-page
  counter because no page was probed.
- Preserve the existing per-page scan loop for ranges fully covered by the
  append buffer.

## Compatibility Impact

No SQL, public API, file format, storage-engine routing, or single-file
lifecycle behavior changes. The slice only short-circuits a cache validation
that would already return failure.

## Scope And Implications

- Affected subsystem: first-party storage packed index-entry append-cache
  validation.
- DDL metadata routing, MariaDB handler behavior, durable page formats, and
  public API: unchanged.
- Dependency, license, binary-size impact: none.
- Risk: rejecting before scanning is only safe because the old behavior also
  rejected on the first missing buffered page and did not update the cache
  memo. The precheck only applies when full coverage is impossible.

## Test And Verification Plan

- Extend the packed-tail memoization hook so a header range extending beyond
  the append buffer records a missing-page blocker without incrementing scanned
  pages.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Missing append-buffer tail ranges reject without page-by-page scan probes.
- The prepared-insert aggregate packed-tail scan-page counter drops to zero or
  records only fully buffered scanned pages.
- Existing storage and routed storage-engine tests pass.

## Verification Results

- `git diff --check` passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  reported no changes.
- `cmake --build --preset dev --target mylite_storage_test` passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  passed in `510.85s`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed in `527.07s`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  reported `packed index tail-append scan pages = 0` and
  `packed index tail-append missing-page blockers = 17`, with the prepared
  insert step at `315.112 us/op` on this VPS.

# Branch Tail Overlay Scan Breakdown

## Problem

After packed-tail missing-range prechecks, the prepared-insert component
benchmark still reports `2` branch-tail overlay scans and `48` scan reads. The
VPS does not have `sample`, `perf`, `gdb`, `pstack`, or `eu-stack`, so the
storage counters are the best available profiling signal. The aggregate read
count does not show whether those scans walk ordinary row pages, branch/index
structure pages, concrete append-tail overlay pages, or other skip-page types.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/mylite-storage/src/storage.c::index_branch_tail_has_live_overlay()`
  scans pages after the largest child page id in a branch root and treats a
  matching index-entry page or same-table row-state page as a live overlay.
- The same scanner treats ordinary row pages, catalog/blob/autoincrement/free
  pages, maintained index roots, index leaf pages, and branch pages as
  non-overlay skip pages.
- Existing benchmark output reports total scan passes and total page reads, but
  does not classify the scanned page kinds.

## Design

- Keep the existing aggregate branch-tail overlay scan counters.
- Add test-hook counters for:
  - scanned index-entry pages;
  - scanned row-state pages;
  - skipped row pages;
  - skipped maintained index structure pages;
  - skipped other page types;
  - concrete overlay hits.
- Print those counters in the prepared-insert component benchmark.
- Add a focused storage test hook that exercises row-page and index-structure
  skip classification without requiring a full SQL workload.

## Compatibility Impact

No SQL, API, routing, durable file-format, or lifecycle behavior changes. This
slice only adds `MYLITE_STORAGE_TEST_HOOKS` diagnostics and benchmark output.

## Scope And Implications

- Affected subsystem: first-party branch-tail overlay planning diagnostics.
- DDL metadata routing, public C API, file lifecycle, and storage-engine
  routing: unchanged.
- Dependency, license, and binary-size impact: small first-party test-hook code
  only, compiled out of non-test-hook builds.

## Test And Verification Plan

- Add storage hook coverage for branch-tail overlay page-kind counters.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Branch-tail overlay aggregate counters remain available.
- Prepared-insert benchmark output shows the page-kind breakdown for remaining
  branch-tail overlay reads.
- Storage and routed storage-engine tests pass.

## Verification Results

- `git diff --check` passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  reported no changes.
- `cmake --build --preset dev --target mylite_storage_test` passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  passed in `326.61s`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed in `344.62s`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  reported branch-tail overlay scans/read pages at `2` / `48`, split into
  `46` row-page skips, `2` index-structure skips, and zero index-entry scans,
  row-state scans, other skips, or overlay hits. The prepared insert step was
  `294.818 us/op` on this VPS.

## Risks And Unresolved Questions

- This slice does not remove the two remaining scans; it makes the next
  optimization choice evidence-backed.

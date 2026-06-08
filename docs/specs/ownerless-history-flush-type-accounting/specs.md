# Ownerless History Flush Type Accounting

## Problem

Ownerless autocommit profiling shows that rollback-segment-space history flush
work is a major remaining cost. The previous page-type profile reported the
flush total and each page-type bucket, but CI did not fail if a future counter
or InnoDB classification change let those numbers drift apart. That would make
the next performance slice tune against incomplete attribution evidence.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `trx_t::write_serialisation_history()` records the ownerless
  rollback-segment-space dirty-page flush total after the history MTR commits
  and before the ownerless history page-write lock is released.
- `buf_flush_wait_space_flushed()` scopes the page-type profile to the current
  thread while it waits for the target rollback-segment tablespace to reach the
  requested LSN.
- `buf_flush_list_space()` increments one page-type bucket only after a page in
  the target tablespace is actually flushed, so the bucket sum should match the
  same wait's returned flushed-page total. Unknown or unexpected page types are
  counted under the explicit `other` bucket.

## Design

Keep ownerless and InnoDB behavior unchanged. Harden only
`mylite_embedded_performance_probe` when
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` is enabled:

- Sum the undo-log, index, FSP header, XDES, inode, allocated, system,
  transaction-system, and other ownerless history-flush page buckets.
- Fail the stats-enabled probe if the bucket sum differs from the total
  ownerless history-flush page count.
- Emit compact summary keys for the bucket sum, per-insert bucket sum, and
  bucket-sum ratio so CI logs expose the invariant without requiring raw-key
  post-processing.

The default stats-off throughput probe does not run this accounting check.

## Compatibility Impact

No SQL, C API, PHP API, storage, directory-lifecycle, or ownerless coordination
behavior changes. This is a production-build performance-probe invariant.

## Native Storage Impact

No native page, redo, undo, checkpoint, or tablespace format change. The
rollback-segment-space flush remains the native proof used before releasing the
ownerless history page-write lock.

## Test Plan

- Build the production embedded performance probe.
- Run a reduced stats-enabled production embedded performance probe and verify
  the new accounting keys are present and the probe passes.
- Run focused production ownerless SQL selectors that cover commit visibility,
  read refresh, local writes, and live reclaim.
- Run the ownerless hook crash selectors that exercise page-visible publish and
  checkpoint boundaries.
- Run production format and whitespace checks.

## Acceptance Criteria

- A stats-enabled production embedded performance probe fails on mismatch
  between total ownerless history-flush pages and the sum of the page-type
  buckets.
- CI-visible summary output includes raw bucket-sum, per-insert bucket-sum, and
  ratio keys for ownerless autocommit attribution.
- Existing focused ownerless correctness coverage still passes.

## Verification Results

Local verification on 2026-06-09 used production embedded builds:

- `tools/require-cmake-release-build build/php-embedded-prod` passed.
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded` passed.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe` passed.
- A stats-enabled production probe passed with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=100`,
  `MYLITE_PERF_INSERT_ITERATIONS=100`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`. It reported `281` total
  ownerless history flush pages and `281` page-type bucket pages, with
  `mylite_perf_summary_ownerless_autocommit_write_history_ownerless_flush_type_page_ratio=1.0000`.
  The same sample split the total into `181` undo-log pages, `100`
  `FIL_PAGE_TYPE_SYS` pages, and zero index, FSP header, XDES, inode,
  allocated, transaction-system, or other pages.
- Focused production ownerless SQL selectors passed: `commit-race`,
  `prepared-committed-read`, `local-write-first-read`, and `live-reclaim`.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test` passed, and the
  `visible-publish-crash` and `visible-checkpoint-crash` hook selectors passed.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

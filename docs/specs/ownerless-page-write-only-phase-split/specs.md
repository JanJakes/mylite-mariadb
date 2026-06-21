# Ownerless Page-Write-Only Phase Split

## Problem

Ownerless bulk insert performance work needs production-path attribution for
the steady-state write path. The existing embedded performance probe can emit
first-statement versus remaining-statement page-write phase summaries, but the
snapshot is only captured when `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` is
enabled.

That diagnostic mode intentionally disables some stats-off fast paths, including
held native-support publish skips, so it can overstate page-publish and
commit-log costs. A page-write-only run keeps page-publish diagnostics off and
therefore measures the production dispatch path, but before this slice it only
emitted aggregate page-write counters with no first/remaining split.

## Source Findings

- `packages/libmylite/tests/embedded_performance_probe.c` stores first bulk
  statement snapshots in `bulk_insert_first_stats`.
- `measure_bulk_autocommit_insert()` already calls
  `capture_bulk_first_stats()` after the first statement when a first-stats
  pointer is provided.
- The ownerless bulk call passed that pointer only when page-publish stats were
  enabled.
- `emit_ownerless_bulk_autocommit_phase_summary()` already emits the remaining
  page-write commit-log, redo-leave, no-dirty-loop, and no-dirty page-publish
  rows used by `tools/embedded-performance-rollup`.

## Design

Capture ownerless first-statement snapshots when either page-publish stats or
page-write stats are enabled. For page-write-only runs, provide zeroed
page-publish, database, page-log, and commit-visibility arrays so the existing
phase summary can still print the page-write rows without enabling
page-publish diagnostics.

Add a CI production report:

- `large-row-ownerless-page-write-attribution.log`;
- `MYLITE_PERF_OWNERLESS_PAGE_WRITE_STATS=1`;
- same 1000-row, 100-row-per-statement shape as the existing large-row
  ownerless page-publish attribution report.

The production-build guard requires the new step to use the production PHP
embedded build and to keep page-publish and append diagnostics disabled. The
rollup self-test includes the new report shape and verifies that remaining
page-write columns are visible even when undo and `mysql_query()` phase rows
are absent.

## Compatibility Impact

No SQL behavior, public C API, PHP API, storage format, page-version WAL,
checkpoint, recovery, locking, or directory lifecycle behavior changes. This is
probe and CI observability only.

## Build And Performance Impact

The CI embedded performance job runs one additional reduced production
performance probe. Runtime impact is limited to CI timing visibility; normal
library builds and product code are unchanged.

The page-write-only probe preserves production ownerless page-publish dispatch
because it does not enable page-publish diagnostics.

## Verification Plan

- Build `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Run a reduced page-write-only probe and verify
  `mylite_perf_summary_ownerless_autocommit_bulk_remaining_page_write_*` rows
  are emitted.
- Run `tools/embedded-performance-rollup-test`.
- Run `tools/check-ci-production-builds`.
- Run the tool CTest selector for embedded performance rollup and CI
  production-build guards.
- Run format and whitespace checks.

## Acceptance Criteria

- Page-write-only ownerless bulk probes emit first and remaining phase summary
  rows.
- The embedded performance rollup surfaces remaining page-write costs for the
  new production-path report.
- The CI guard fails if the report stops using production builds or accidentally
  enables page-publish diagnostics.
- No ownerless storage, concurrency, or SQL compatibility claims are changed.

## Verification Results

Passed:

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe`
- Reduced production page-write-only probe:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=1000`,
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100`,
  `MYLITE_PERF_OWNERLESS_PAGE_WRITE_STATS=1`,
  `build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`
- `tools/embedded-performance-rollup-test`
- `tools/check-ci-production-builds`
- `bash -n tools/embedded-performance-rollup
  tools/embedded-performance-rollup-test tools/check-ci-production-builds`
- `ctest --preset prod -R
  '^tools\.(embedded-performance-rollup|ci-production-builds)$'
  --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

The reduced page-write-only probe confirmed
`mylite_perf_ownerless_page_publish_stats=0` and
`mylite_perf_ownerless_page_write_stats=1`. It emitted first and remaining
phase rows, including remaining steady-state values:

- `page_write_commit_log_ms_per_statement=0.329`;
- `page_write_commit_log_redo_leave_ms_per_statement=0.080`;
- `page_write_redo_leave_hook_ms_per_statement=0.062`;
- `page_write_commit_log_no_dirty_loop_ms_per_statement=0.158`;
- `page_write_commit_log_no_dirty_page_publish_ms_per_statement=0.087`;
- `page_write_native_support_transaction_publish_skipped_per_statement=100.000`.

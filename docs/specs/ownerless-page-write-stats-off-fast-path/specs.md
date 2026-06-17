# Ownerless Page-Write Stats-Off Fast Path

## Problem

Ownerless write throughput remains below ordinary embedded InnoDB throughput.
The larger remaining costs are native page publication, history/commit work,
and page-version WAL append decisions, but the page-write performance helper
still does avoidable work in the normal stats-disabled runtime path.

Several page-write timing sites set `start_ns = 0` when page-write performance
stats are disabled, then call `ownerless_page_write_perf_add_elapsed()`. That
helper still reloads the stats-enabled atomic through `ownerless_page_write_perf_add()`
before it can determine no counter will be updated. The same pattern affects
scope destructors for ownerless page-write enter, leave, publish, and publish
scan timing.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `ownerless_page_write_perf_scope` stores `m_start_ns = 0` when the
  `ownerless_page_write_perf_stats_enabled` flag is disabled.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `ownerless_page_write_perf_add_elapsed()` receives that zero sentinel but
  still calls `ownerless_page_write_perf_add()`, which reloads the same
  disabled flag before doing nothing.
- The current production attribution sample after live native-support index
  skip reports ownerless autocommit page-write publish around
  `0.092 ms/insert`, page-write commit-log around `0.145 ms/insert`, and
  page-write no-dirty loop around `0.073 ms/insert`. Normal production and
  WordPress timing runs keep the page-write performance counters disabled.

## Design

Return immediately from `ownerless_page_write_perf_add_elapsed()` when
`start_ns == 0`. When `start_ns` is non-zero, keep checking the enabled flag
before recording elapsed time so disabling counters before scope exit still
suppresses the final update.

Add focused ownerless SQL coverage that resets page-write performance counters
with stats disabled, runs the native-support proof insert workload, reads the
first page-write perf counter, and verifies it remains zero. Existing
stats-enabled production probes still exercise the enabled path and summary
output.

## Scope And Non-Goals

In scope:

- MariaDB-derived ownerless MTR page-write performance helper;
- focused ownerless SQL regression coverage for stats-disabled counter state;
- production stats-off and stats-enabled probe evidence.

Out of scope:

- page-version WAL format changes;
- reducing history-proof page count;
- SYS page deltas or blind native-support elision;
- redo/checkpoint reconciliation or DDL/file lifecycle recovery.

## Compatibility Impact

No SQL, public C API, PHP API, wire-protocol, storage format, page-log format,
checkpoint, recovery, or directory-layout behavior changes. This is an internal
stats-disabled fast path for first-party diagnostics.

## Directory And Lifecycle Impact

No durable files, shared-memory layout, checkpoint files, or directory
lifecycle rules change.

## Native Storage Impact

No native InnoDB page, redo, undo, purge, checkpoint, or recovery behavior
changes. Native latch release and page-write lock release ordering are
unchanged.

## Build And Performance Impact

The MariaDB embedded archive must be rebuilt after editing `mtr0mtr.cc`.
Stats-disabled ownerless page-write elapsed scopes avoid a second relaxed
atomic load and function path after the caller already recorded a zero timing
sentinel. The expected throughput impact is small; this is a bounded cleanup
that keeps larger proof and commit costs visible.

## Test Plan

- Rebuild the MariaDB embedded archive.
- Build production embedded performance and ownerless SQL targets.
- Run the focused native-support page WAL elision SQL selector to exercise the
  stats-disabled counter assertion plus existing proof/rebuild coverage.
- Run adjacent history-proof and visibility selectors.
- Run reduced stats-off and stats-enabled production performance probes.
- Run hook crash selectors around visible publication/checkpoint.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- Stats-disabled page-write elapsed calls return without rechecking the disabled
  stats flag.
- Focused ownerless SQL coverage proves disabled page-write counters stay zero
  through the native-support proof insert workload.
- Stats-enabled probes continue to emit page-write timing rows.
- Focused ownerless correctness and hook selectors pass.

## Verification Results

Local production verification used the `php-embedded-prod`, `ownerless-test-hooks`,
and `ownerless-stress` build presets after rebuilding MariaDB embedded with
the documented `MinSizeRel` baseline.

Passed:

- `tools/mariadb-embedded-build build`
- `cmake --build --preset php-embedded-prod --target mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test single-owner-native-support-page-wal-elision`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test single-owner-history-wal-proof`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test prepared-committed-read`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test active-pin-reclaim-boundary`
- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test`
- hook selectors `visible-publish-crash`, `visible-checkpoint-crash`, and
  `page-publish-before-append-crash`
- `cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test`
- `ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-stress$' --output-on-failure`
- `tools/check-ci-production-builds`
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`
- `tools/require-cmake-release-build build/php-embedded-prod`
- `tools/require-cmake-release-build build/ownerless-test-hooks`
- `tools/require-cmake-release-build build/ownerless-stress`
- `cmake --build --preset format-check-prod`
- `git diff --check`

The stats-off throughput sample used
`MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`, `MYLITE_PERF_SELECT_ITERATIONS=50`, and
`MYLITE_PERF_INSERT_ITERATIONS=2000`. It reported ordinary autocommit
`3673.54 ops/s`, ownerless autocommit `1046.79 ops/s`, ratio `0.2850`,
ordinary bulk `11610.77 rows/s`, ownerless bulk `3006.27 rows/s`, and bulk
ratio `0.2589`. That sample did not show a clear throughput win from this
small helper cleanup.

The stats-enabled attribution sample used
`MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`, `MYLITE_PERF_SELECT_ITERATIONS=20`,
`MYLITE_PERF_INSERT_ITERATIONS=300`, and
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`. It continued to emit page-write
summary rows, including `0.137 ms/insert` in page-write publish,
`0.207 ms/insert` in page-write commit-log, and `0.095 ms/insert` in the
no-dirty commit-log loop, with ownerless autocommit at ratio `0.2666`.

## Risks

- This does not address the main remaining ownerless write-path cost. Broader
  completion still needs a safe replacement for current history-proof
  publication or native redo/checkpoint reconciliation.
- Timing samples may be noisy enough that this cleanup does not show a clear
  stats-off throughput improvement.

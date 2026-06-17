# Ownerless Commit Visibility Stats-Off Fast Path

## Problem

Normal production, CI timing, WordPress PHPUnit, and default embedded
performance runs keep ownerless commit-visibility attribution disabled. The
ownerless transaction commit path already records `start_ns = 0` when those
stats are disabled, but elapsed helpers still reload the disabled atomic before
doing no work. The same commit block also reloaded the enabled flag for each
visible-fast or conservative-flush reason counter.

This is not the dominant ownerless write-throughput cost. Current attribution
still points at native InnoDB commit work, history-proof page publication,
page-log append, and broader redo/checkpoint reconciliation. This slice removes
avoidable diagnostics overhead from the normal stats-disabled commit path
without changing commit visibility, page publication, lock release, or recovery
semantics.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc` owns ownerless commit-visibility
  attribution for visible-fast publication, conservative flush reasons, log
  flush time, page-publication time, page-visible publication time, lock
  release time, and total ownerless commit time.
- The ownerless commit block already loads
  `ownerless_commit_visibility_stats_enabled` to decide whether to capture a
  timing start. Disabled stats therefore pass a zero timing sentinel to
  `ownerless_commit_visibility_add_elapsed()`.
- The elapsed helper still reloaded the disabled stats flag before returning,
  and reason counters reloaded the same flag for each counter.

## Design

`ownerless_commit_visibility_add_elapsed()` now returns immediately when
`start_ns == 0`. When `start_ns` is non-zero, it still rechecks the enabled flag
before recording elapsed time, preserving the previous behavior where disabling
stats before scope exit suppresses the update.

The ownerless commit block now snapshots the commit-visibility stats-enabled
flag once, uses it to decide whether to take timing samples, and passes that
snapshot to reason-counter updates. Enabled stats keep the same counters and
counter order; disabled stats avoid repeated relaxed atomic loads.

Focused ownerless SQL coverage resets commit-visibility counters while stats
are disabled, executes a real ownerless write, reads the counters, and verifies
representative count and elapsed counters remain zero. Existing enabled
visible-fast coverage continues to prove the attribution path.

## Compatibility Impact

No SQL result, public C API, PHP API, mysqli adapter, wire-protocol,
directory-layout, storage-format, page-version WAL, checkpoint, redo, undo, or
native recovery behavior changes. This is an internal diagnostics fast path.

## Directory And Lifecycle Impact

No durable files, shared-memory fields, checkpoint records, WAL records, or
runtime directory lifecycle rules change.

## Native Storage Impact

Native InnoDB commit, log flush, page flush, page-visible publication, and lock
release ordering are unchanged. Only diagnostics counter gating changes.

## Build And Performance Impact

Because the implementation edits MariaDB-derived InnoDB code, the embedded
MariaDB archive must be rebuilt before production embedded targets. The
stats-disabled ownerless commit path avoids repeated relaxed atomic loads and
elapsed helper work after the caller already proved stats are disabled. The
expected throughput impact is small; this keeps the production path cleaner
while the larger proof-volume and native commit costs remain visible.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive.
- Build production ownerless SQL and embedded performance targets.
- Run focused ownerless SQL selectors for visible-fast multi-row inserts,
  explicit-transaction visible-fast commit proof, and native-support WAL proof.
- Run reduced stats-off and stats-enabled production performance probes.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- Disabled commit-visibility elapsed calls return before rechecking the
  disabled stats flag.
- Ownerless commit reason counters use one stats-enabled snapshot per commit
  block.
- Focused SQL coverage proves representative commit-visibility counters stay
  zero while stats are disabled.
- Enabled visible-fast attribution and production probe output continue to
  work.

## Verification Results

Local production verification used `build/mariadb-embedded` with the documented
`MinSizeRel` cache and `build/php-embedded-prod` production targets.

Passed:

- `tools/mariadb-embedded-build build`
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-multi-row-insert-visible-fast-path`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  explicit-transaction-visible-fast-commit`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-native-support-page-wal-elision`
- Reduced stats-off production probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=100`,
  `MYLITE_PERF_INSERT_ITERATIONS=2000`, and
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=4`.
- Reduced stats-enabled production probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=50`,
  `MYLITE_PERF_INSERT_ITERATIONS=200`,
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=4`,
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`, and
  `MYLITE_PERF_OWNERLESS_DATABASE_STATS=1`.
- Filtered stats-enabled attribution sample with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=80`,
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=4`,
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`, and
  `MYLITE_PERF_OWNERLESS_DATABASE_STATS=1`.
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

The stats-off probe reported ordinary autocommit at `3721.89 ops/s`,
ownerless autocommit at `1422.45 ops/s`, ratio `0.3822`; ordinary four-row
bulk rows at `6427.19 rows/s`, ownerless four-row bulk rows at `4307.89 rows/s`,
ratio `0.6703`; ordinary active runtime reconnect at `2.226 ms`, ownerless
active runtime reconnect at `0.830 ms`; ordinary explicit-transaction inserts
at `3271.89 ops/s`, ownerless explicit-transaction inserts at `2684.72 ops/s`,
ratio `0.8205`.

The stats-enabled attribution sample still emitted commit-visibility counters:
the filtered 80-row run reported autocommit `commit_visibility_fast=80`,
`commit_visibility_flush=0`, total commit-visibility time `2.503 ms`, publish
transaction-pages time `0.218 ms`, publish-visible time `1.456 ms`, and lock
release time `0.794 ms`. Four-row bulk reported
`commit_visibility_fast_per_statement=1.000` and
`commit_visibility_flush_per_statement=0.000`, with the existing `6.000` page
versions per statement, `2.000` native-support pages published per statement,
and `4.000` native-support pages elided per statement.

## Risks And Follow-Up

This is a bounded diagnostics hot-path cleanup. It does not replace the
remaining native history-proof publication, redo/checkpoint reconciliation,
DDL/file lifecycle recovery, active-reader pressure policy, or external
MariaDB/RQG stress work needed before ownerless concurrency can be called
complete.

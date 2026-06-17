# Ownerless Page-Write Publish Perf Snapshot

## Problem

Normal production, CI timing, WordPress PHPUnit, and default embedded
performance runs keep ownerless page-write performance counters disabled. The
ownerless page publish path already uses zero timing sentinels so disabled
elapsed helpers return cheaply, but `mtr_t::ownerless_page_write_publish()`
still reloaded the disabled page-write perf flag for each publish counter and
each timing start inside every published page.

This path runs for each ownerless page-version publication. Current production
attribution still shows write cost in page publication, page-log append, and
native row/undo work, so the stats-disabled publish path should avoid repeated
diagnostics-only flag work.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::ownerless_page_write_publish()`
  owns ownerless page image publication from committed mini-transactions.
- `ownerless_page_write_perf_add_elapsed()` already returns immediately when
  the caller passes `start_ns == 0`.
- `ownerless_page_write_perf_scope` previously loaded the page-write perf flag
  itself, and the publish function separately loaded the same flag before
  space lookup, scratch allocation, page copy, checksum initialization,
  page-version hook, and scratch free timing.
- Publish-call and publish-buffer-reuse counters also called the perf add
  helper, which reloaded the same disabled flag before doing nothing.

## Design

Snapshot `ownerless_page_write_perf_enabled()` once at the start of
`ownerless_page_write_publish()`. Use that boolean for:

- the publish-call counter;
- the publish-total perf scope;
- space lookup, allocation, copy, checksum, hook, and free timing starts; and
- publish-buffer reuse counters.

When the snapshot is false, diagnostics-only counters and timing starts are
skipped. When the snapshot is true, the existing counters and elapsed helpers
still run. Elapsed helpers keep their final enabled check, so disabling stats
before scope end still suppresses the final elapsed update when a non-zero
start was captured.

## Compatibility Impact

No SQL, public C API, PHP API, mysqli adapter, wire-protocol, directory-layout,
storage-format, page-version WAL, checkpoint, redo, undo, or recovery behavior
changes. This is an internal diagnostics fast path.

## Directory And Lifecycle Impact

No durable files, shared-memory fields, checkpoint records, WAL records, or
runtime directory lifecycle rules change.

## Native Storage Impact

Native mini-transaction commit, page image copy/checksum, page-version
publication, history-proof marking, and ownerless lock release ordering are
unchanged. Only diagnostics gating changes.

## Build And Performance Impact

The change edits MariaDB-derived InnoDB MTR code, so the embedded MariaDB
archive must be rebuilt before production targets. The expected impact is a
small production-path reduction in relaxed atomic loads and perf-helper calls
per published page when page-write perf stats are disabled. It does not reduce
page-version count, native-support history-proof count, page-log append work,
or native row/undo commit cost.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive.
- Build production ownerless SQL and embedded performance targets.
- Run focused ownerless SQL selectors for multi-row visible-fast inserts,
  history WAL proof, native-support WAL elision, and active-reader pressure.
- Run reduced stats-off and stats-enabled production performance probes and
  compare page-write publish counters, buffer-reuse counters, page-version
  counts, native-support counts, and commit-visibility rows.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- `ownerless_page_write_publish()` snapshots the page-write perf enabled state
  once per publish call.
- Stats-disabled publish calls avoid repeated page-write perf flag checks and
  direct perf counter helper calls.
- Stats-enabled attribution still emits publish-call, publish-total,
  subphase, and buffer-reuse counters.
- Focused ownerless SQL coverage preserves page-version counts,
  native-support publication counts, and visible-fast commit counters.

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
  single-owner-history-wal-proof`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-native-support-page-wal-elision`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  active-reader-pressure`
- Filtered stats-enabled attribution probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=500`,
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=4`,
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`, and
  `MYLITE_PERF_OWNERLESS_DATABASE_STATS=1`.
- Reduced stats-off production probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=100`,
  `MYLITE_PERF_INSERT_ITERATIONS=2000`, and
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=4`.

The stats-enabled sample preserved the expected publication shape:
`3.008` page versions per insert, `2.004` native-support pages published per
insert, `1.000` history-proof rollback-segment page and `1.000`
history-proof undo page per insert, `1.000` bulk commit-visibility fast path
per statement, and `0.000` bulk commit-visibility flushes per statement.
Page-write publish counters still emitted: `2004` publish calls for `500`
inserts, `3.008` buffer-reuse hits per insert, `0.000` buffer-reuse misses per
insert, `0.084 ms/insert` publish total, and `0.066 ms/insert` publish hook
time.

The stats-off probe reported ordinary autocommit at `3188.12 ops/s`,
ownerless autocommit at `1625.00 ops/s`, ratio `0.5097`; ordinary four-row
bulk rows at `12729.61 rows/s`, ownerless four-row bulk rows at
`4255.36 rows/s`, ratio `0.3343`; ordinary explicit-transaction inserts at
`3560.64 ops/s`, ownerless explicit-transaction inserts at `2457.07 ops/s`,
ratio `0.6901`.

## Risks And Follow-Up

This is a bounded diagnostics hot-path cleanup. It does not address the larger
native history-proof publication, page-log append, row/undo MTR commit, or
redo/checkpoint reconciliation costs.

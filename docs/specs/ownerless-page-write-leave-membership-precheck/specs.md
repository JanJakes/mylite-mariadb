# Ownerless Page-Write Leave Membership Precheck

## Problem

After the no-dirty leave-exhaustion fast path, current production attribution
still shows visible ownerless write cost in the no-dirty page-write commit-log
loop. The loop stops calling `ownerless_page_write_leave()` once the MTR-owned
page-write vector is empty, but while that vector is non-empty it still calls
the helper for every X/SX page memo slot. Non-member slots then enter the
helper, count/measure a leave attempt, and return after the helper proves the
page was not recorded in the vector.

The release rule is already vector membership based. A caller-side membership
precheck can avoid helper work for pages that cannot release an ownerless
page-write lock.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::commit_log()` owns the
  no-dirty memo release loop and sees each page memo slot before native latch
  release.
- `mtr_t::ownerless_page_write_has_mtr_page()` checks whether a page is present
  in `m_ownerless_page_write_mtr_pages` without mutating the vector.
- `mtr_t::ownerless_page_write_leave()` only releases an ownerless page-write
  lock after the page is present in that same vector and
  `ownerless_page_write_forget_mtr_page()` removes it.
- Therefore a no-dirty loop slot that fails `ownerless_page_write_has_mtr_page()`
  cannot release an ownerless page-write lock through the helper.

## Design

In the no-dirty commit-log loop, keep the existing loop-local
`ownerless_page_leave` flag that tracks whether the vector is non-empty. Before
calling `ownerless_page_write_leave()`, also call
`ownerless_page_write_has_mtr_page(*bpage)`. Only member pages enter the leave
helper; after the helper returns, refresh `ownerless_page_leave` from the vector
state as in the exhaustion fast path.

This preserves member release behavior and only skips helper calls that would
have returned without releasing anything.

## Compatibility Impact

No SQL, public C API, PHP API, mysqli adapter, wire-protocol, directory-layout,
storage-format, page-version WAL, checkpoint, redo, undo, or recovery behavior
changes. This is an internal ownerless InnoDB hook fast path.

## Directory And Lifecycle Impact

No durable files, shared-memory fields, checkpoint records, WAL records, or
runtime directory lifecycle rules change.

## Native Storage Impact

Native mini-transaction memo release, page latch release, redo-latch release,
flush-list handling, page publication, and ownerless lock release ordering are
unchanged. Pages recorded in the MTR page-write vector still call the existing
leave helper before native latch release.

## Build And Performance Impact

The change edits MariaDB-derived InnoDB MTR code, so the embedded MariaDB
archive must be rebuilt before production targets. The expected impact is
bounded to no-dirty commit-log loops where the MTR page-write vector is
non-empty and later X/SX page memo slots are not vector members. Stats-enabled
leave-call counts may fall because impossible leave attempts are no longer
counted.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive.
- Build production ownerless SQL and embedded performance targets.
- Run focused ownerless SQL selectors for multi-row visible-fast inserts,
  history WAL proof, native-support WAL elision, and active-reader pressure.
- Run reduced stats-off and stats-enabled production performance probes and
  compare page-write commit-log, no-dirty-loop, page-version, native-support,
  and commit-visibility summary rows.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- Non-member no-dirty page slots do not call `ownerless_page_write_leave()`.
- Member page-write release behavior remains unchanged.
- Focused ownerless SQL coverage preserves page-version counts,
  native-support publication counts, and visible-fast commit counters.
- Production probes do not show a correctness-count regression.

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
Page-write commit-log total sampled at `0.150 ms/insert`, publish at
`0.089 ms/insert`, and no-dirty loop at `0.074 ms/insert`. The leave-call
count did not materially drop in this simple insert workload (`2610` calls for
500 inserts), indicating that most X/SX slots observed while the vector is
non-empty are actual members for this shape. Treat this as a defensive
caller-side skip for other memo-slot mixes, not a proven dominant-path win.

The stats-off probe reported ordinary autocommit at `3268.63 ops/s`,
ownerless autocommit at `1427.02 ops/s`, ratio `0.4366`; ordinary four-row
bulk rows at `10960.93 rows/s`, ownerless four-row bulk rows at
`3632.37 rows/s`, ratio `0.3314`; ordinary active runtime reconnect at
`0.793 ms`, ownerless active runtime reconnect at `0.879 ms`.

## Risks And Follow-Up

This is a targeted no-dirty loop cleanup. It does not remove required
history-proof/native-support page publication, replace redo/checkpoint
reconciliation, or address native row/undo MTR commit cost.

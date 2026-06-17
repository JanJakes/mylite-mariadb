# Ownerless Page-Write Leave Exhaustion Fast Path

## Problem

Current production attribution still shows ownerless autocommit write overhead
in the page-write commit-log path. A 500-row stats-enabled sample at
`15302f88` reported `0.226 ms/insert` in page-write commit-log handling,
including `0.128 ms/insert` in the no-dirty loop. The same sample reported
`3.000` page-write commit-log calls per insert but only a small number of
MTR-owned page-write entries can require ownerless release work.

The no-dirty commit-log loop already checks whether an MTR has any
ownerless-owned page-write entries before it starts releasing memo slots.
However, that decision was fixed for the whole loop. Once the last ownerless
MTR page was forgotten, later X/SX page memo slots still called
`ownerless_page_write_leave()`, which opened the diagnostics scope and returned
after seeing the vector was empty.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::commit_log()` owns the
  no-dirty memo release loop.
- `mtr_t::ownerless_page_write_leave()` releases an ownerless page-write lock
  only after `ownerless_page_write_forget_mtr_page()` proves the current page
  was recorded in `m_ownerless_page_write_mtr_pages`.
- `ownerless_page_write_leave()` already returns without release when that
  vector is null or empty.
- The no-dirty loop computed `ownerless_page_leave` once before the loop and
  therefore kept calling the helper even after the vector became empty.

## Design

Make the no-dirty loop's `ownerless_page_leave` flag mutable. After each
`ownerless_page_write_leave()` call, refresh the flag from
`m_ownerless_page_write_mtr_pages` and stop invoking the helper once the vector
is empty.

This keeps the existing release rule for member pages and only skips calls that
the helper would already return from without releasing anything. Native memo
release and page latch release order are unchanged.

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
unchanged. The loop only stops calling the ownerless leave helper after no MTR
page-write lock remains to release.

## Build And Performance Impact

The change edits MariaDB-derived InnoDB MTR code, so the embedded MariaDB
archive must be rebuilt before production targets. The expected impact is
bounded to no-dirty commit-log loops with multiple page latch memo slots after
the MTR-owned page-write vector is exhausted. Stats-enabled leave-call counts
may fall because impossible leave attempts are no longer counted.

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

- The no-dirty loop stops invoking `ownerless_page_write_leave()` once the
  MTR-owned page vector is empty.
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
- Reduced stats-off production probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=100`,
  `MYLITE_PERF_INSERT_ITERATIONS=2000`, and
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=4`.
- Filtered stats-enabled attribution probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=500`,
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=4`,
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`, and
  `MYLITE_PERF_OWNERLESS_DATABASE_STATS=1`.

The stats-off probe reported ordinary autocommit at `3738.59 ops/s`,
ownerless autocommit at `1410.66 ops/s`, ratio `0.3773`; ordinary four-row
bulk rows at `13077.75 rows/s`, ownerless four-row bulk rows at
`3488.72 rows/s`, ratio `0.2668`; ordinary active runtime reconnect at
`0.834 ms`, ownerless active runtime reconnect at `0.997 ms`.

The stats-enabled sample preserved the expected publication shape:
`3.008` page versions per insert, `2.004` native-support pages published per
insert, `1.000` history-proof rollback-segment page and `1.000`
history-proof undo page per insert, `1.000` bulk commit-visibility fast path
per statement, and `0.000` bulk commit-visibility flushes per statement.
The sampled no-dirty loop moved from the pre-slice `0.128 ms/insert` at
`15302f88` to `0.066 ms/insert`, while page-write commit-log total was
`0.201 ms/insert`.

## Risks And Follow-Up

This is a targeted no-dirty loop cleanup. It does not remove required
history-proof/native-support page publication, replace redo/checkpoint
reconciliation, or address the native row/undo MTR commit cost.

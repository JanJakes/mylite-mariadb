# Ownerless Redo Leave Subphase Attribution

## Problem Statement

The reduced ownerless 100-row bulk probe after inline MTR page tracking still
shows redo leave as the largest page-write commit-log subphase:

- `1815` ownerless redo leave calls across 10 bulk statements;
- `6.301 ms` in `page_write_commit_log_redo_leave`;
- `2.355 ms` in the MyLite redo-state leave hook counters.

The difference is the native MariaDB `log_write_up_to()` interval plus wrapper
overhead in `mtr_t::ownerless_redo_leave()`. The current performance probe
cannot separate native redo write waiting from MyLite redo-state progress-latch
work, so the next optimization target is ambiguous.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`) with MyLite branch head
  `8b6a9d7e9000889961f4bd762d59ebd8a2e563b8`.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::ownerless_redo_leave()`
  first calls `log_write_up_to(lsn, false)` when `m_commit_lsn` is nonzero,
  then either calls `mylite_ownerless_innodb_redo_written_and_leave()` for a
  reserved redo range or `mylite_ownerless_innodb_redo_leave()` for the
  fallback path.
- `mariadb/storage/innobase/log/log0log.cc::log_write_up_to()` uses
  `group_commit_lock::acquire()`; for non-durable writes, a no-op path is
  possible when another writer has already advanced the target write LSN, but
  that cost is not visible from current MyLite counters.
- `packages/libmylite/src/database.cc` records redo-state hook time in
  `OWNERLESS_DATABASE_PERF_REDO_LEAVE_NS`, but this does not include the native
  `log_write_up_to()` call that precedes the hook.
- `packages/libmylite/tests/embedded_performance_probe.c` already mirrors
  page-write perf counters for commit-log subphases and compact bulk,
  autocommit, and explicit-transaction summaries.

## Design

Add stats-enabled page-write perf counters around the existing redo leave
subphases:

- native log-write call count and elapsed time for `log_write_up_to(lsn,
  false)`;
- zero-LSN leave count when no native log write is attempted;
- MyLite redo-state hook elapsed time;
- written-range hook call count;
- fallback leave hook call count.

Emit those counters in detailed page-write stats and compact summaries for
ownerless autocommit, explicit transaction, and 100-row bulk probe phases.

This is attribution only. It must not add a fast skip, change native redo write
ordering, change ownerless redo-state publication, checkpoint persistence,
page-visible publication, page-version WAL records, SQL behavior, public API,
or directory layout.

## Compatibility Impact

No compatibility surface changes. The slice changes production probe output and
stats-enabled instrumentation only.

## Directory And Native Storage Impact

No durable file, shared-memory, WAL, checkpoint, native tablespace, or runtime
lifecycle format changes. Native MariaDB redo and MyLite redo-state hooks keep
their existing call order.

## Binary Size, License, And Dependency Impact

The slice touches one upstream-derived InnoDB source file and first-party test
probe mirrors. It adds no dependency and only a few stats counters.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive after editing `mtr0mtr.cc`.
- Build production embedded and PHP performance/ownerless SQL targets.
- Run a reduced stats-enabled production probe and verify new detailed and
  compact redo-leave subphase keys appear.
- Run focused ownerless primitive/open-close and visible-fast SQL coverage.
- Run production build guards, format-check, and `git diff --check`.

## Acceptance Criteria

- Existing redo leave order remains `log_write_up_to()` before redo-state hook.
- Detailed stats split native log-write time from redo-state hook time.
- Compact summaries expose the split for bulk, autocommit, and explicit
  transaction probes.
- Focused ownerless correctness coverage passes.

## Verification Results

Passed:

- `tools/mariadb-embedded-build build`
- `cmake --build --preset embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test
  mylite_ownerless_primitives_test mylite_embedded_open_close_test`
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test
  mylite_ownerless_primitives_test mylite_embedded_open_close_test`
- `ctest --preset embedded-prod -R
  '^libmylite\.(ownerless-primitives|embedded-open-close)$'
  --output-on-failure`
- `build/embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-multi-row-insert-visible-fast-path`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-multi-row-insert-visible-fast-path`
- `tools/check-ci-production-builds`
- `cmake --build --preset format-check-prod`
- `git diff --check`
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test mylite_ownerless_primitives_test`
- `ctest --preset ownerless-test-hooks -R
  '^libmylite\.(ownerless-primitives|ownerless-single-owner-(history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path))$'
  --output-on-failure`

The stats-enabled embedded production probe used:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=20
MYLITE_PERF_INSERT_ITERATIONS=1000
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
build/embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

It preserved the expected ownerless 100-row bulk write volume:

- `2.000` page-version records per statement;
- `4.500` page-log append calls per statement;
- `2.000` native-support published pages per statement;
- `90.100` native-support elided pages per statement;
- `1.000` fast commit-visible publications per statement;
- `0.000` commit-visibility flushes per statement;
- `180.500` deferred latest-checkpoint coalesces per statement.

The same sample split the 100-row bulk redo leave path as:

- aggregate `page_write_commit_log_redo_leave`: `0.671 ms/statement`;
- native `log_write_up_to()`: `181.500` calls and `0.362 ms/statement`;
- MyLite redo-state hook: `0.281 ms/statement`;
- written-range hook calls: `181.500` per statement;
- fallback hook calls: `0.000` per statement;
- zero-LSN leaves: `0.000` per statement.

The matching `php-embedded-prod` probe shape preserved the same count profile
and reported `0.853 ms/statement` aggregate redo leave, with
`0.468 ms/statement` in native `log_write_up_to()` and `0.341 ms/statement`
in the MyLite redo-state hook.

## Risks And Follow-Up

This does not reduce runtime overhead by itself. It should identify whether the
next runtime slice belongs in native redo write avoidance, redo-state hook
bookkeeping, or a higher-level reduction in ownerless mini-transaction redo
leave frequency.

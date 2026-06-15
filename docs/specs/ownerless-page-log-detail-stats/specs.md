# Ownerless Page-Log Detail Stats

## Problem

Ownerless production probes need page-log append timing that is close enough to
stats-off behavior to guide CI performance decisions. The existing append stats
path also classified every appended page image by InnoDB page type and updated
index-page identity buckets. A local 500-row production attribution sample with
stats enabled spent `47.029 ms` in `page_type_stats` during ownerless
autocommit inserts, while the stats-off production sample showed a materially
smaller ownerless/ordinary gap. That made the probe useful for byte attribution
but too expensive as the default timing signal.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- InnoDB stores the page type at `FIL_PAGE_TYPE` offset `24` and defines the
  page-type values used by MyLite diagnostics in
  `mariadb/storage/innobase/include/fil0fil.h`.
- MyLite page-log append diagnostics live in
  `packages/libmylite/src/ownerless_page_log.cc`. The cheap append counters
  cover calls, bytes, append stages, payload encoding, and payload-format
  totals. The expensive detail path is `record_append_page_type_stats()`, which
  reads the page type and updates per-page-class and index identity counters.
- The embedded performance probe enables ownerless attribution from
  `packages/libmylite/tests/embedded_performance_probe.c` through
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`.

## Design

Split page-log append diagnostics into two levels:

1. `mylite_ownerless_page_log_set_append_perf_stats_enabled(1)` continues to
   enable the existing append stats and, for direct primitive coverage,
   preserves the previous default of detailed page-type stats on.
2. `mylite_ownerless_page_log_set_append_detail_perf_stats_enabled(0)` disables
   only the page-type and index-identity attribution work. It does not disable
   append calls, append-stage timing, payload byte counters, payload-format
   totals, or page-log sync/scan diagnostics.
3. The embedded performance probe enables append stats but defaults detail stats
   off unless `MYLITE_PERF_OWNERLESS_PAGE_LOG_DETAIL_STATS=1` is set.

The default probe still reports the same output keys. Detail-only page-class
and index-identity counters report zero when detail is disabled, which makes the
timing mode explicit without changing the output schema.

## Compatibility Impact

This slice is diagnostics-only. It does not change SQL behavior, public
`libmylite` database APIs, page-log record format, page-log recovery,
checkpointing, locking, native storage files, or database-directory layout.
Existing primitive tests that enable append stats directly continue to get the
old detailed counters by default.

## Embedded Lifecycle And Storage Impact

No durable state is added. The new flag is process-local diagnostic state and is
cleared when append stats are disabled. It has no effect on ownerless open,
close, recovery, native checkpoint promotion, or active-reader WAL retention.

## Test Plan

- Add primitive coverage proving append counters and bytes continue while
  detail stats are disabled, page-type counters remain zero, and re-enabling
  detail restores index page attribution.
- Run the production primitive CTest selector.
- Run reduced production performance probes with default detail-off attribution
  and with `MYLITE_PERF_OWNERLESS_PAGE_LOG_DETAIL_STATS=1`.
- Run production-build guards and static checks before committing.

## Acceptance Criteria

- Default stats-enabled probes print
  `mylite_perf_ownerless_page_log_detail_stats=0`.
- The default stats-enabled probe still emits append calls, append bytes,
  payload encoding, and payload-format counters.
- Detail page-class counters are zero unless the new environment flag is set.
- Opting into detail restores page-class counters without changing page-log
  correctness tests.

# Ownerless Bulk Exec Result Attribution

## Problem

The production embedded performance probe already reports ownerless bulk
insert page-log, page-write, commit-visibility, SQL handler, InnoDB handler,
and deep InnoDB counters. A reduced 100-row bulk sample still had a larger
per-statement elapsed interval than those exposed buckets explained. The probe
did not emit the existing `mylite_exec()` / `mysql_query()` attribution for the
bulk insert phases, so CI could not show whether the missing time was inside
SQL text execution or in post-query result/status handling.

## Design

Reuse the existing exec-result performance counters around the ordinary and
ownerless bulk autocommit insert phases when ownerless attribution mode is
enabled:

- reset the existing exec-result counters at the same post-DDL boundary as the
  other insert counters;
- enable them only for the timed bulk row-list loop;
- emit the existing detailed `*_exec_result_*` rows for ordinary and ownerless
  bulk phases;
- emit compact per-statement summary rows for calls, `mysql_query()`,
  store-result, and status-update time.

No runtime behavior, SQL policy, WAL format, checkpoint ordering, page
publication, or recovery semantics change.

## Compatibility Impact

None. This is diagnostic-only performance attribution in the embedded
performance probe. Production library behavior is unchanged unless the probe
explicitly enables the existing counters.

## Verification Plan

- Build `mylite_embedded_performance_probe` with a production preset.
- Run a reduced stats-enabled probe with
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100`.
- Confirm ordinary and ownerless bulk exec-result rows and compact
  per-statement summaries are present.
- Run formatting and diff checks.

## Acceptance Criteria

- CI ownerless attribution logs expose ordinary and ownerless bulk
  `mysql_query()` time per statement.
- Existing ownerless bulk page-log, page-write, commit-visibility, and
  default-checked bulk-start summaries remain present.
- No compatibility matrix entry claims a throughput win from this
  diagnostic-only slice.

## Verification Results

Passed:

- `cmake --build --preset embedded-prod --target
  mylite_embedded_performance_probe`
- Reduced attribution probe:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=1
MYLITE_PERF_INSERT_ITERATIONS=100
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
build/embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

The reduced probe emitted the new rows. In that run, ordinary bulk reported
`mylite_perf_summary_ordinary_autocommit_bulk_mysql_query_ms_per_statement=2.836`.
Ownerless bulk reported
`mylite_perf_summary_ownerless_autocommit_bulk_mysql_query_ms_per_statement=2.023`,
`mylite_perf_summary_ownerless_autocommit_bulk_status_update_ms_per_statement=0.208`,
`mylite_perf_ownerless_insert_autocommit_bulk_commit_visibility_total_ms=0.338`,
`mylite_perf_ownerless_insert_autocommit_bulk_page_log_append_total_ms=0.244`,
and
`mylite_perf_summary_ownerless_autocommit_bulk_page_write_commit_log_ms_per_statement=0.224`.
The same run preserved
`mylite_perf_summary_ownerless_autocommit_bulk_default_checked_bulk_starts_per_statement=1.000`.

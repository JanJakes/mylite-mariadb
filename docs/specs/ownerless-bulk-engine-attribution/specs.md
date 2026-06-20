# Ownerless Bulk Engine Attribution

## Problem

The ownerless bulk insert probe now reports page-log, page-write,
commit-visibility, redo-state, SQL-handler, InnoDB-handler, and deep InnoDB
counters. The ordinary bulk baseline only reported row/statement throughput and
`mylite_exec()` timing. That made the remaining ownerless/ordinary bulk gap hard
to assign: a slow ownerless `mysql_query()` interval could be native row insert
work, native commit/history work, MyLite page publication, or unreported ordinary
baseline engine work.

## Source Findings

- `packages/libmylite/tests/embedded_performance_probe.c` times ordinary and
  ownerless row-list inserts in `measure_bulk_autocommit_insert()` after a
  post-DDL counter reset.
- The ownerless bulk phase already emits `emit_sql_handler_perf_stats()`,
  `emit_innodb_handler_perf_stats()`, `emit_innodb_deep_perf_stats()`, page-log,
  page-write, commit-visibility, database, and compact bulk summaries.
- The ordinary point-select and single-row insert phases already enable SQL,
  handler, and deep InnoDB counters for baseline comparison. Ordinary bulk did
  not.
- MariaDB/InnoDB deep counters used by this probe are instrumentation around
  native paths such as `trx_commit_for_mysql()` / history write in
  `mariadb/storage/innobase/trx/trx0trx.cc`, row insert entry in
  `mariadb/storage/innobase/row/row0mysql.cc`, clustered insert in
  `mariadb/storage/innobase/row/row0ins.cc`, and mini-transaction commit in
  `mariadb/storage/innobase/mtr/mtr0mtr.cc`.

MariaDB base ref remains `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

## Design

When `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` is enabled, the performance
probe now enables the existing SQL-handler, InnoDB-handler, and deep InnoDB
counters around the ordinary timed bulk insert loop as well as the ownerless
loop. It then emits:

- raw ordinary bulk SQL-handler, InnoDB-handler, and deep InnoDB rows;
- a compact ownerless/ordinary deep-InnoDB comparison for key bulk row/commit,
  undo, clustered B-tree, history-write, and default-checked bulk-start buckets;
- both per-row and per-statement forms so large row-list and small row-list
  probes remain comparable.

No production SQL path, WAL format, checkpoint policy, page-version publication,
or ownerless concurrency behavior changes.

## Compatibility Impact

None. This is diagnostic-only performance attribution in the embedded
performance probe. It changes only probe output when the opt-in
page-publish-attribution environment variable is enabled.

## Directory And Lifecycle Impact

None. The probe still creates and removes its temporary MyLite database
directory through the existing performance harness.

## Test Plan

- Build `mylite_embedded_performance_probe` with the production PHP embedded
  preset.
- Run a reduced production stats-enabled probe with a multi-row bulk shape and
  confirm ordinary raw engine counters plus compact ownerless-minus-ordinary
  bulk rows are emitted.
- Run formatting and diff checks.

## Acceptance Criteria

- Ordinary bulk attribution includes SQL-handler, InnoDB-handler, and deep
  InnoDB rows.
- Compact summaries include ownerless-minus-ordinary bulk engine deltas for
  commit, row insert, clustered insert, undo report, and default-checked bulk
  counters.
- Existing ownerless page-log, page-write, commit-visibility, redo, and
  exec-result bulk summaries remain present.
- No throughput improvement is claimed from this diagnostic-only slice.

## Verification Results

Passed:

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe`
- Reduced production attribution probe:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=1
MYLITE_PERF_INSERT_ITERATIONS=100
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

The reduced probe emitted ordinary bulk engine rows, including
`mylite_perf_ordinary_insert_autocommit_bulk_innodb_handler_write_row_calls=100`,
`mylite_perf_ordinary_insert_autocommit_bulk_innodb_deep_trx_commit_for_mysql_total_ms=0.010`,
and
`mylite_perf_ordinary_insert_autocommit_bulk_innodb_deep_row_insert_for_mysql_total_ms=0.587`.

The compact comparison rows reported representative per-statement deltas:

- `mylite_perf_summary_ownerless_minus_ordinary_autocommit_bulk_trx_commit_for_mysql_ms_per_statement=0.547`
- `mylite_perf_summary_ownerless_minus_ordinary_autocommit_bulk_row_insert_ms_per_statement=-0.347`
- `mylite_perf_summary_ownerless_minus_ordinary_autocommit_bulk_row_ins_clust_low_ms_per_statement=-0.386`
- `mylite_perf_summary_ownerless_minus_ordinary_autocommit_bulk_trx_undo_report_ms_per_statement=-0.132`
- `mylite_perf_summary_ownerless_minus_ordinary_autocommit_bulk_innodb_default_checked_bulk_starts_per_statement=1.000`

Existing ownerless bulk summaries remained present, including
`mylite_perf_summary_ownerless_autocommit_bulk_page_versions_per_statement=2.000`,
`mylite_perf_summary_ownerless_autocommit_bulk_native_support_published_pages_per_statement=2.000`,
`mylite_perf_summary_ownerless_autocommit_bulk_page_write_commit_log_redo_leave_ms_per_statement=0.002`,
and
`mylite_perf_summary_ownerless_autocommit_bulk_mysql_query_ms_per_statement=1.962`.
The same reduced run reported ordinary bulk at `74763.34 rows/s`, ownerless
bulk at `24867.41 rows/s`, and an ownerless/ordinary ratio of `0.3326`; this is
sample evidence for attribution only, not a throughput claim.

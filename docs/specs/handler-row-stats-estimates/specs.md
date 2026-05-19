# Handler Row Stats Estimates

## Problem

The MyLite handler currently answers every `HA_STATUS_VARIABLE` request by
calling `mylite_storage_count_rows()`. That storage API is exact, but it scans
all table row pages and row-state pages. MariaDB calls `handler::info()` with
`HA_STATUS_VARIABLE` during ordinary SELECT optimization, so indexed point reads
pay a full table scan before execution.

The local performance baseline sample on 2026-05-19 showed prepared secondary
index reads spending most sampled time in:

- `JOIN::optimize_inner()`
- `make_join_statistics()`
- `ha_mylite::info(HA_STATUS_VARIABLE)`
- `mylite_storage_count_rows()`
- `scan_table_row_pages()`

That is incompatible with SQLite-like point-read performance.

## Source Findings

- MariaDB 11.8.6 source ref:
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/include/my_base.h` defines `HA_STATUS_VARIABLE` as the handler
  request for variable statistics including `records`, file lengths, and mean
  record length. `HA_STATUS_NO_LOCK` explicitly allows a slightly stale copy.
- `mariadb/sql/sql_select.cc` calls `table->file->info(HA_STATUS_VARIABLE)`
  from optimizer paths that only need planning statistics.
- `mariadb/sql/handler.h` defines `HA_STATS_RECORDS_IS_EXACT` separately.
  MyLite does not set that table flag, so `stats.records` is not promised as an
  exact row count to the SQL layer.
- `mariadb/storage/rocksdb/ha_rocksdb.cc` uses approximate cached storage
  statistics and avoids reporting zero rows for optimizer planning because zero
  can confuse plans.

## Design

`ha_mylite::info()` should stop using `mylite_storage_count_rows()` for durable
`HA_STATUS_VARIABLE` requests. Instead it should publish cheap approximate
statistics derived from the primary `.mylite` file size:

- `stats.records`: approximate row count for optimizer costing, never exact;
- `stats.data_file_length`: primary file size;
- `stats.index_file_length`: zero for now because data and indexes share one
  file;
- `stats.mean_rec_length`: table record length when available.

`COUNT(*)`, table scans, index reads, DML, duplicate checks, FK checks, and
direct storage tests continue using exact row and index storage paths. The
first-party `mylite_storage_count_rows()` API remains exact for tests and
storage callers that need that behavior.

Volatile `MEMORY` / `HEAP` routed tables can keep the exact in-memory count
because that does not scan the durable primary file.

## Compatibility Impact

This changes SQL-visible handler metadata that depends on `stats.records`, such
as `SHOW TABLE STATUS` row estimates. That is acceptable for a MySQL/MariaDB
compatible engine because engines without `HA_STATS_RECORDS_IS_EXACT`, including
InnoDB-style engines, can report estimates. SQL result correctness must not
depend on the estimate.

## Single-File And Lifecycle Impact

No file-format change and no new companion files are introduced. The handler
uses existing primary-file metadata and leaves durable row storage untouched.

## Test And Verification Plan

- Rebuild the storage-smoke MariaDB archive and storage-smoke targets.
- Run storage unit tests.
- Run the storage-engine compatibility harness.
- Run the local performance baseline and confirm indexed SELECT timings improve
  because `ha_mylite::info()` no longer scans rows.
- Run formatting and diff checks before committing.

## Acceptance Criteria

- Durable `ha_mylite::info(HA_STATUS_VARIABLE)` no longer calls
  `mylite_storage_count_rows()`.
- Exact storage row-count APIs and SQL `COUNT(*)` remain correct.
- Routed storage compatibility tests pass.
- The local performance baseline records a material reduction in indexed SELECT
  latency.

## Risks

- Estimates can change optimizer plan choices for multi-table queries. That is
  already the contract for non-exact handler statistics, but later slices should
  add real table/index statistics or ANALYZE-style metadata.
- File-size-derived estimates are database-wide rather than per-table. This is
  intentionally temporary and should be replaced by maintained per-table stats
  when the pager/catalog metadata grows.
- Follow-up slice `stat-free-handler-estimates` replaced the primary-file-size
  stat proxy with a fixed nonzero planning estimate so ordinary optimizer stats
  requests do not perform filesystem stats.

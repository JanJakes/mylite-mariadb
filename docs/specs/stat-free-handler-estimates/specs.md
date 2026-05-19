# Stat-Free Handler Estimates

## Problem

After persistent stat-table reads were disabled, the direct point-select hot
path still enters `ha_mylite::info(HA_STATUS_VARIABLE)` during MariaDB
optimization. MyLite already avoids exact row scans there, but the durable path
still calls `mysql_file_stat()` for every variable-stats request to derive a
coarse database-wide file-size estimate.

That syscall is now visible in local samples of direct point-select loops. It
does not provide table-owned statistics, because the primary file contains all
MyLite tables, catalog pages, row pages, index pages, and row-state history.
Until MyLite maintains real table/index statistics in the primary file, the
file size is a costly and weak proxy.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/include/my_base.h` defines `HA_STATUS_VARIABLE` as variable handler
  metadata for rows, deleted rows, file lengths, and mean row length.
- `mariadb/sql/sql_select.cc` calls `table->file->info(HA_STATUS_VARIABLE)`
  from optimizer planning paths, so this must be cheap for ordinary reads.
- `mariadb/sql/opt_sum.cc` only treats handler row counts as exact when
  `HA_STATS_RECORDS_IS_EXACT` is set. MyLite does not set that flag for durable
  tables.
- `mariadb/sql/opt_range.cc::get_sweep_read_cost()` uses
  `stats.data_file_length` in range-cost math, so MyLite should keep the
  durable estimate nonzero even when it is approximate.
- `mariadb/sql/handler.cc` copies handler stats into table status metadata; the
  values are SQL-visible estimates for engines that do not advertise exact row
  counts.
- `mariadb/storage/mylite/ha_mylite.cc::info()` currently answers durable
  `HA_STATUS_VARIABLE` requests from `mysql_file_stat()` on the primary
  `.mylite` path.

## Design

- Remove the durable `mysql_file_stat()` call from
  `ha_mylite::info(HA_STATUS_VARIABLE)`.
- Return a constant nonzero durable planning estimate large enough for
  point-predicate queries to keep favoring index access:
  - `stats.records = mylite_stats_default_record_estimate`;
  - `stats.data_file_length = mylite_stats_default_data_file_length`;
  - `stats.index_file_length = 0`, because rows and indexes share the primary
    file;
  - `stats.mean_rec_length` remains the MariaDB table share record length when
    available.
- Keep volatile `MEMORY` / `HEAP` routed tables on exact in-memory counts,
  because those counts are cheap and do not touch the primary file.
- Leave exact SQL results untouched. `COUNT(*)`, row scans, index reads,
  duplicate checks, FK checks, and storage tests keep using exact storage paths.

## Compatibility Impact

`SHOW TABLE STATUS` and optimizer estimates for durable MyLite tables become a
constant coarse estimate rather than a database-file-size estimate. That is
compatible with MySQL/MariaDB handler semantics because MyLite does not claim
`HA_STATS_RECORDS_IS_EXACT`.

The tradeoff is deliberate: the previous estimate was database-wide and often
misleading for individual tables. A later statistics slice should maintain
per-table and per-index stats inside the `.mylite` file, with ANALYZE-style
refresh behavior if needed.

## Single-File And Lifecycle Impact

No file-format change and no new companion files. The change avoids a read-only
filesystem syscall during planning and does not alter storage durability or
recovery.

## Public API And File-Format Impact

No public C API or file-format change.

## Storage-Engine Routing Impact

All durable MyLite-routed engines (`MYLITE`, omitted/default routed to MyLite,
`InnoDB`, `MyISAM`, `Aria`) share the same stat-free handler estimate.
`BLACKHOLE` still reports discard-row estimates, and `MEMORY` / `HEAP` keep
exact volatile row counts.

## Binary-Size And Dependency Impact

No dependency change. Removing `mysql_file_stat()` from the MyLite handler lets
`ha_mylite.cc` drop the Performance Schema file wrapper include, but any linked
size impact is expected to be negligible.

## Test And Verification Plan

- Rebuild storage-smoke handler and performance targets.
- Run the storage-engine compatibility harness.
- Run the local performance baseline and confirm direct point-select samples no
  longer show `mysql_file_stat()` / `my_stat()` from `ha_mylite::info()`.
- Run formatting and whitespace checks.

## Acceptance Criteria

- Durable `ha_mylite::info(HA_STATUS_VARIABLE)` performs no filesystem stat.
- Durable handler estimates remain nonzero and large enough for representative
  point-select planning to keep index access.
- Exact SQL result paths and storage row-count APIs remain unaffected.
- Storage-engine compatibility checks pass.

## Verification Results

- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`
- `tools/mylite-compat-harness run storage-engine`
- `build/storage-smoke-dev/tools/mylite_perf_baseline 1000 10000`
- `build/storage-smoke-dev/tools/mylite_perf_baseline 1000 100000`
- `git clang-format --diff HEAD -- mariadb/storage/mylite/ha_mylite.cc packages/libmylite/tests/embedded_storage_engine_test.c`
- `git diff --check`

Local perf sample after implementation, 1000 rows and 100000 iterations:

- direct primary-key point selects: `27.644 us/op`
- prepared primary-key point selects: `11.982 us/op`
- direct secondary exact selects: `67.478 us/op`
- prepared secondary exact selects: `38.523 us/op`
- direct published-leaf secondary exact selects: `65.602 us/op`
- prepared published-leaf secondary exact selects: `40.007 us/op`

The sampled 100000-iteration run no longer shows `my_stat()` or
`mysql_file_stat()` below `ha_mylite::info()`. A representative storage-engine
EXPLAIN regression test now asserts that secondary predicates keep using the
secondary key under the stat-free durable estimate.

## Risks

- Coarser estimates can affect join or range plan choices. A too-small default
  biases the optimizer toward scans, so representative secondary exact-read
  baseline coverage is required until real MyLite-owned table/index statistics
  exist.
- SQL-visible table status estimates become less correlated with primary file
  growth. They were already database-wide rather than table-specific.

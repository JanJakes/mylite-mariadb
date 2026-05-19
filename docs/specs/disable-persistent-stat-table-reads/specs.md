# Disable Persistent Stat Table Reads

## Problem

After storage-local exact-read optimizations, direct point-select profiling is
dominated by MariaDB optimizer metadata startup rather than row lookup. The
post-bucket `sample` run shows ordinary `SELECT` execution entering:

- `read_statistics_for_tables()`
- `open_stat_tables()`
- `open_system_tables_for_read()`
- `ha_discover_table()`
- `mylite_discover_table()`
- `mylite_storage_begin_read_statement()`

Those calls try to open MariaDB's persistent `mysql.table_stats`,
`mysql.column_stats`, and `mysql.index_stats` tables for each statement. MyLite
does not persist those server-owned stat tables in the `.mylite` catalog, so
the work is both expensive and unproductive in the current embedded profile.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_statistics.cc::read_statistics_for_tables_if_needed()`
  routes ordinary `SELECT`, `INSERT`, `UPDATE`, `DELETE`, `REPLACE`, and
  selected other commands into `read_statistics_for_tables()`.
- `mariadb/sql/sql_statistics.cc::read_statistics_for_tables()` returns early
  when `thd->variables.use_stat_tables == NEVER`; otherwise user tables without
  a populated `TABLE_SHARE::stats_cb` cause `open_stat_tables()`.
- `mariadb/sql/sql_statistics.cc::open_stat_tables()` opens the three
  persistent `mysql.*_stats` tables through `open_system_tables_for_read()`.
- `mariadb/sql/table.cc` treats `mysql.*_stats` names as internal statistics
  tables and notes that `mysql.*` is for internal purposes.
- MyLite storage architecture already states that required `mysql.*` system
  surfaces must be MyLite-backed metadata or read-only virtual surfaces, not
  inherited Aria/datadir tables.
- `packages/libmylite/src/database.cc::configure_connection()` already applies
  MyLite-specific session defaults for file-backed storage-smoke sessions:
  empty `sql_mode`, `default_storage_engine=MYLITE`, and
  `enforce_storage_engine=MYLITE`.

## Design

- Set `SET SESSION use_stat_tables=NEVER` during file-backed MyLite connection
  configuration.
- Keep the setting at session scope so raw MariaDB defaults and unrelated
  builds are not changed globally.
- Keep `ha_mylite::info(HA_STATUS_VARIABLE)` as the source of cheap planning
  estimates for routed MyLite tables.
- Do not remove SQL statistics code or persistent-stat table commands in this
  slice. Broader stat-table or histogram virtual surfaces remain separate
  compatibility work.

## Compatibility Impact

MyLite sessions stop reading inherited persistent statistics tables by default.
That can change optimizer estimates compared with a full MariaDB server that
has populated `mysql.*_stats`, but it matches the current embedded product
shape: those server-owned tables are not durable MyLite application state.

Users can still execute supported `SET SESSION use_stat_tables=...` statements
if they deliberately want MariaDB's behavior for an experiment. MyLite does not
yet provide MyLite-backed persistent statistics, so the default remains
`NEVER`.

## Single-File And Lifecycle Impact

No file-format change and no new companion files. The change avoids accidental
reads of server-owned stat tables from the transient MariaDB runtime directory.

## Public API And File-Format Impact

No public C API or file-format change.

## Storage-Engine Routing Impact

All file-backed routed tables benefit because the setting is applied before
normal SQL executes through `libmylite`. `:memory:` sessions keep their existing
configuration path.

## Binary-Size And Dependency Impact

No new dependency and no immediate size change. A later build-profile slice can
consider compiling out more persistent statistics and histogram code once the
remaining SQL surfaces are explicitly covered.

## Test And Verification Plan

- Add storage-engine smoke coverage asserting the MyLite session default is
  `@@session.use_stat_tables = NEVER`.
- Rebuild storage-smoke targets.
- Run storage-engine compatibility harness.
- Run the performance baseline and compare point-select/discovery hot paths.
- Run formatting and whitespace checks.

## Acceptance Criteria

- File-backed `libmylite` connections default to
  `@@session.use_stat_tables = NEVER`.
- Direct point-select profiling no longer shows `open_stat_tables()` as the
  dominant path.
- Storage-engine compatibility checks pass.

## Verification Results

- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`
- `tools/mylite-compat-harness run storage-engine`
- `build/storage-smoke-dev/tools/mylite_perf_baseline 1000 10000`
- `build/storage-smoke-dev/tools/mylite_perf_baseline 1000 100000`
- `git clang-format --diff HEAD -- packages/libmylite/src/database.cc packages/libmylite/tests/embedded_storage_engine_test.c`
- `git diff --check`

Local perf sample after implementation, 1000 rows and 100000 iterations:

- direct primary-key point selects: `30.274 us/op`
- prepared primary-key point selects: `12.692 us/op`
- direct secondary exact selects: `66.110 us/op`
- prepared secondary exact selects: `39.478 us/op`
- direct published-leaf secondary exact selects: `66.552 us/op`
- prepared published-leaf secondary exact selects: `40.482 us/op`

The post-change sample no longer shows `open_stat_tables()` or
`open_system_tables_for_read()` under the direct point-select hot path. The
remaining prominent planning cost is `ha_mylite::info(HA_STATUS_VARIABLE)`
calling `my_stat()` for primary-file size estimates.

## Risks

- This relies on a MariaDB session variable rather than removing the upstream
  code path. User SQL can re-enable persistent stat-table reads.
- Some optimizer plans may differ from full MariaDB with populated persistent
  statistics. That is acceptable until MyLite has file-owned table/index
  statistics.

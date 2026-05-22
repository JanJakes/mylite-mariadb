# Skip Disabled Update Statistics

## Problem

File-backed MyLite sessions set `use_stat_tables=NEVER`, so MariaDB's
persistent `mysql.*_stats` tables are disabled by default. Prepared update
profiling still shows `read_statistics_for_tables()` under
`Sql_cmd_update::prepare_inner()` because the update path calls the statistics
entry point on every execution before that function returns early for the
disabled mode.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc` sets file-backed MyLite sessions to
  `SET SESSION use_stat_tables=NEVER`.
- `mariadb/sql/sql_update.cc::Sql_cmd_update::prepare_inner()` calls
  `read_statistics_for_tables_if_needed()` during every single-table update
  prepare pass.
- `mariadb/sql/sql_statistics.cc::read_statistics_for_tables()` already
  returns immediately when `thd->variables.use_stat_tables == NEVER`.
- Local prepared-update samples still show the function call frame on the hot
  prepared row-DML path.

## Design

Gate the update prepare statistics call at the caller when
`get_use_stat_tables_mode(thd) == NEVER`.

This preserves MariaDB's existing statistics behavior for every mode that can
read persistent statistics while avoiding the function entry, DBUG frame, and
table-list setup work for MyLite's default stat-free embedded profile.

## Affected Subsystems

- MariaDB single-table and multi-table `UPDATE` context analysis.
- MyLite prepared row-DML performance when persistent statistics are disabled.

No parser, handler, storage, catalog, file-format, or public API behavior
changes.

## Compatibility Impact

No SQL semantics change is intended. `use_stat_tables=NEVER` already means
MariaDB must not read persistent statistics tables for planning. Other
`use_stat_tables` modes still call the existing statistics reader.

## Single-File And Embedded Lifecycle Impact

No durable state, sidecar, lock, recovery, or lifecycle change. The slice keeps
the embedded default away from inherited persistent statistics tables.

## Public API And File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Binary-Size And Dependency Impact

No new dependency. Binary-size impact should be neutral to negligible.

## Tests And Verification

- `git diff --check`
- `git clang-format --diff -- mariadb/sql/sql_update.cc`
- Rebuild the storage-smoke MariaDB embedded archive with static MyLite
  storage.
- Build storage-smoke embedded storage-engine and performance targets.
- Run focused storage-smoke embedded storage-engine tests.
- Run `mylite_perf_baseline --phase=prepared-update-components 1000 1000000`.
- Capture a focused prepared-update sample and confirm the disabled statistics
  reader frame is absent or materially reduced.

Completed verification:

- `git diff --check` passed.
- `git clang-format --diff -- mariadb/sql/sql_update.cc` passed.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke
  tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC` passed and
  rebuilt `libmariadbd.a` at 20.21 MiB.
- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_storage_engine_test mylite_perf_baseline` passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'libmylite.embedded-storage-engine' --output-on-failure` passed 1/1 test.
- `ctest --preset storage-smoke-dev --output-on-failure` passed 10/10 tests.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-update-components 1000 1000000` measured the prepared
  update step at 1.712 us/op.
- The focused sample in
  `/tmp/mylite-skip-disabled-update-statistics.sample.txt` no longer shows
  `read_statistics_for_tables*` frames. The remaining SQL-layer cost is still
  dominated by `open_tables_for_query()` and `JOIN::prepare()`.

## Acceptance Criteria

- `UPDATE` still reads persistent statistics when `use_stat_tables` is not
  `NEVER`.
- File-backed MyLite prepared updates with the default stat-free mode do not
  enter `read_statistics_for_tables_if_needed()` from `prepare_inner()`.
- Focused tests pass.
- Performance notes record the observed prepared-update impact.

## Risks And Unresolved Questions

- This does not solve repeated table open, MDL, or `JOIN::prepare()` work for
  prepared DML. Those remain larger prepared-execution reuse problems.

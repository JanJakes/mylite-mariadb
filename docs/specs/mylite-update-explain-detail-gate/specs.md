# MyLite Update Explain Detail Gate

## Problem

Prepared primary-key updates still spend visible time in MariaDB's
`Update_plan::save_explain_update_data()` path even when the statement is an
ordinary execution and no `EXPLAIN`, `ANALYZE`, or slow-log explain output is
requested. The hot work includes `QUICK_RANGE_SELECT::get_explain()` allocation
and MRR detail formatting that are useful only when plan details will be
printed.

## Source Findings

- Base line: MariaDB 11.8.6, import ref
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_update.cc` calls
  `query_plan.save_explain_update_data()` before executing ordinary
  single-table `UPDATE`, and again on the explicit `EXPLAIN UPDATE` path.
- `mariadb/sql/sql_delete.cc::Update_plan::save_explain_data_intern()` eagerly
  appends possible keys, builds `Explain_quick_select` with
  `select->quick->get_explain()`, and appends MRR details.
- `mariadb/sql/log.cc` prints full slow-log explain output only when
  `LOG_SLOW_VERBOSITY_EXPLAIN` is enabled.
- MyLite's embedded SQL policy treats server metadata and process-list surfaces
  as unsupported, while ordinary `EXPLAIN` plan output remains supported.

## Design

- Keep upstream behavior unchanged unless MyLite schema hooks are active.
- Preserve full plan detail collection for explicit `EXPLAIN`, `ANALYZE`, and
  slow-log explain or engine-stat collection.
- For ordinary MyLite executions, still create the `Explain_update` /
  `Explain_delete` node and command tracker used by existing execution code,
  but skip key-list, quick-info, and MRR explain-detail allocation.
- Leave ordinary `EXPLAIN UPDATE` output covered by a routed-storage test so
  compatibility-facing plan output keeps using the original detail path.

## Scope

In scope:

- MyLite-active ordinary single-table `UPDATE` / `DELETE` explain detail
  allocation.
- Routed `EXPLAIN UPDATE` regression coverage.
- Prepared update performance evidence.

Out of scope:

- Changing MariaDB range optimization or `QUICK_RANGE_SELECT` construction.
- Changing SELECT explain behavior.
- Adding SHOW EXPLAIN support as a MyLite embedded surface.

## Compatibility Impact

Explicit `EXPLAIN UPDATE`, `EXPLAIN DELETE`, `ANALYZE`, and slow-log explain
detail behavior stays on the upstream path. The optimized path only affects
ordinary execution under MyLite schema hooks, where the detailed runtime node is
not user-visible through supported MyLite APIs.

## Single-File And Lifecycle Impact

No durable storage or companion-file lifecycle change.

## Test Plan

- Add routed-storage coverage that `EXPLAIN UPDATE ... WHERE secondary_key =`
  still reports the expected key.
- Build `mysqlserver`, `mylite_storage_test`,
  `mylite_embedded_storage_engine_test`, and `mylite_perf_baseline`.
- Run focused storage-smoke CTest coverage.
- Run `git diff --check` and `git clang-format --diff`.
- Run `tools/mylite-perf-baseline --phase=prepared-updates 1000 1000000`.

## Acceptance Criteria

- Explicit routed `EXPLAIN UPDATE` still reports the expected key.
- Ordinary MyLite prepared updates no longer sample
  `QUICK_RANGE_SELECT::get_explain()` under
  `Update_plan::save_explain_update_data()`.
- Non-MyLite MariaDB behavior remains unchanged.

## Verification

- `git diff --check`: passed.
- `git clang-format --diff -- mariadb/sql/sql_delete.cc
  packages/libmylite/tests/embedded_storage_engine_test.c`: no changes.
- `cmake --build build/mariadb-mylite-storage-smoke --target mysqlserver`:
  passed with existing upstream missing-override and archive warnings.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`: passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`: prepared primary-key updates measured
  `2.391 us/op`; the sampled run measured `2.381 us/op`.
- A two-second macOS `sample` run over the same phase no longer showed
  `QUICK_RANGE_SELECT::get_explain()` or `explain_append_mrr_info()` under
  `Update_plan::save_explain_update_data()`. Remaining sampled costs were in
  active update rewrite/undo, handler key preparation, MariaDB quick range
  construction/execution, and table/cache lookups.

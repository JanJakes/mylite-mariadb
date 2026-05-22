# Prepared Update Value Shape Cache

## Problem

The prepared-DML execution reuse design calls for caching only immutable shape
decisions before attempting larger cleanup or `JOIN::prepare()` reuse. One
small repeated decision in `Sql_cmd_update::prepare_inner()` is whether the
`UPDATE` value list contains subqueries. That property is structural for a
prepared statement, but the current code scans the value list on every prepare
pass before deciding whether MyLite can elide the generic single-update result
object.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_update.cc::mylite_can_elide_single_update_result()` rejects
  value lists with subqueries by calling
  `mylite_update_values_have_subquery()`.
- `mariadb/sql/sql_update.cc::Sql_cmd_update::prepare_inner()` runs this
  decision on every prepared `UPDATE` execution because DML command objects are
  unprepared after execution today.
- The value-list subquery presence is immutable for a parsed prepared
  statement object. `mariadb/sql/sql_prepare.cc::Prepared_statement::reprepare()`
  prepares a fresh `Prepared_statement copy`, then swaps it into place after
  metadata validation, so a reprepare gets a new `Sql_cmd_update` object from
  the new parse tree.

## Design

- Add a small cache on `Sql_cmd_update` for the value-list subquery result.
- Compute the result through the existing value-list scan the first time the
  command object needs it.
- Reuse the cached result in later prepare passes for the same prepared command
  object.
- Keep the elision gate's remaining table, view, period, returning,
  semi-join, inner-unit, and multitable checks dynamic.

This is intentionally not a table-handle, JOIN, or metadata reuse change.

## Affected Subsystems

- MariaDB single-table `UPDATE` prepare.
- MyLite direct-update result elision.

No handler, storage, catalog, file-format, or public API behavior changes.

## Compatibility Impact

No SQL semantics change is intended. The cache stores only whether the parsed
value-list shape contains a subquery. It does not cache parameter values,
expression results, diagnostics, warnings, table metadata, or row state.

## Single-File And Embedded Lifecycle Impact

No durable state, sidecar, lock, recovery, or lifecycle change.

## Public API And File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Binary-Size And Dependency Impact

Adds two booleans and one small private method to `Sql_cmd_update`; no new
dependency.

## Tests And Verification

- Passed `git diff --check`.
- Passed `git clang-format --diff -- mariadb/sql/sql_update.cc
  mariadb/sql/sql_update.h`.
- Passed `BUILD_DIR=build/mariadb-mylite-storage-smoke
  tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`; archive size:
  20.21 MiB.
- Passed `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_statement_test mylite_embedded_storage_engine_test
  mylite_perf_baseline`.
- Passed `ctest --test-dir build/storage-smoke-dev -R
  'libmylite.embedded-statement|libmylite.embedded-storage-engine'
  --output-on-failure`.
- Passed `ctest --preset storage-smoke-dev --output-on-failure`.
- Ran `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-update-components 1000 1000000`; prepared primary-key
  update step measured 1.690 us/op.

## Acceptance Criteria

- Prepared update behavior remains unchanged.
- Direct text updates and unsupported prepared update shapes keep existing
  fallback behavior.
- The value-list subquery scan is no longer repeated once the same
  `Sql_cmd_update` object has cached the structural result.

## Risks And Unresolved Questions

- This is a small first reuse primitive and will not materially reduce the
  dominant `open_tables_for_query()` / `JOIN::prepare()` cost by itself.

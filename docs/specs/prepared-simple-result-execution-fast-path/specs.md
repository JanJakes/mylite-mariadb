# Prepared Simple Result Execution Fast Path

## Problem

Prepared primary-key point-select loops use result-producing statements that do
not perform DDL, DML, transaction control, schema selection, temporary-table
lifecycle changes, or MyLite statement checkpoints. The current
`execute_statement()` path still enters the generic execution machinery built
for those surfaces on every first step after reset.

That generic path is necessary for prepared DML, DDL, `SET` transaction
controls, `USE`, and temporary-table lifecycle SQL. It is redundant for ordinary
prepared `SELECT` statements whose only work is parameter binding, MariaDB
execution, result binding, and the first fetch.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/libmysqld/libmysql.c::mysql_stmt_execute()` resets stored-result
  state, executes the prepared statement, and prepares the result fetch path
  only when `mysql->field_count` is nonzero.
- `packages/libmylite/src/database.cc::prepare_impl()` already classifies
  prepared SQL for transaction control, storage writes, storage outer
  checkpoints, transaction statement checkpoints, schema selection, schema
  catalog synchronization, and temporary-table lifecycle changes.
- `packages/libmylite/src/database.cc::initialize_statement_metadata()` records
  whether the prepared statement has result columns, and
  `bind_statement_results()` owns result binding for that statement shape.
- The prepared point-select benchmark uses a result-producing `SELECT` with one
  scalar parameter and no lifecycle side effects.

## Design

- Mark prepared statements as `uses_simple_result_execution` only when they have
  result columns and none of the generic MyLite lifecycle surfaces apply:
  transaction control, schema selection, schema catalog synchronization,
  storage writes, storage outer checkpoints, statement checkpoints, or
  transaction statement checkpoints.
- In `execute_statement()`, dispatch those statements directly to a small
  result execution helper after diagnostics and current-row state are cleared.
- The helper performs the same required work for this shape:
  - bind parameters when needed;
  - call `mysql_stmt_execute()`;
  - set MyLite statement and database execution state;
  - bind result buffers;
  - fetch the first row.
- Keep the generic path unchanged for non-result statements, DDL/DML,
  transaction-control statements, schema-selection statements, and any
  statement needing a MyLite checkpoint or lifecycle update.

## Compatibility Impact

Public API behavior remains the same for ordinary prepared result statements:
first `mylite_step()` executes and returns the first row, subsequent steps fetch
more rows or `MYLITE_DONE`, and reset/finalize behavior is unchanged.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, catalog, or companion-file lifecycle
change. Statements that might affect those surfaces are deliberately excluded
from the fast path.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing change. Routed storage reads still happen through MariaDB and the
MyLite storage engine during `mysql_stmt_execute()` / `mysql_stmt_fetch()`.

## Binary-Size And Dependency Impact

Small first-party helper and one statement flag. No dependency or meaningful
binary-size impact.

## Tests And Verification

- Run existing embedded statement coverage, including scalar result, text/BLOB,
  reset-before-drain, large-value, and metadata paths.
- Run routed storage-smoke coverage.
- Run focused prepared point-select, reset-after-row, and prepared-update
  benchmarks.
- Run `git diff --check` and `git clang-format --diff` on `database.cc`.

## Verification Evidence

- `git diff --check`
- `git clang-format --diff -- packages/libmylite/src/database.cc`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_statement_test mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/libmylite/mylite_embedded_statement_test`
- `ctest --test-dir build/storage-smoke-dev -R 'libmylite.embedded-statement|libmylite.embedded-storage-engine' --output-on-failure`
  - 2/2 tests passed.
- `ctest --preset storage-smoke-dev --output-on-failure`
  - 10/10 tests passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-selects 10000 1000000`
  - Prepared primary-key point selects: `9.136 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-select-reset-after-row 10000 1000000`
  - Prepared primary-key point selects reset after row: `9.136 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-updates 10000 1000000`
  - Prepared primary-key updates in one transaction: `2.549 us/op`.

## Acceptance Criteria

- Ordinary result-producing prepared statements that do not need MyLite
  lifecycle machinery use the simple result execution path.
- DML, DDL, transaction-control, schema-selection, temporary-table lifecycle,
  and checkpointed statements remain on the generic path.
- Existing statement and storage-smoke tests pass.
- Local benchmarks record whether the change is measurable.

## Risks And Unresolved Questions

- The fast-path eligibility must stay conservative. Future prepared SQL
  lifecycle classifiers should clear eligibility by setting the existing generic
  flags rather than adding separate checks in the fast path.
- This still executes through MariaDB's prepared statement machinery; larger
  SQLite-like select gains likely require deeper planner/storage integration or
  a narrower first-party execution path.

# Prepared Drained Result Reset Fast Path

## Problem

Prepared primary-key point selects execute a result-producing statement in a
tight bind, row, done, reset, re-execute loop. MyLite already skips an explicit
MariaDB `mysql_stmt_reset()` after successful non-result prepared statements,
but fully drained result statements still pay that reset even though
`fetch_statement_row()` has already freed the result when it reaches
`MYSQL_NO_DATA`.

The sampled 10k-row baseline on this branch measured prepared primary-key point
selects at `9.640 us/op`, leaving result-statement reset overhead as a bounded
prepared API target before broader storage-index work.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc::mylite_reset()` frees an active result,
  then calls `mysql_stmt_reset()` for every executed statement except the
  previous successful non-result fast path.
- `packages/libmylite/src/database.cc::fetch_statement_row()` marks the
  statement done, calls `mysql_stmt_free_result()`, and clears `result_active`
  when `mysql_stmt_fetch()` returns `MYSQL_NO_DATA`.
- `mariadb/libmysqld/libmysql.c::mysql_stmt_execute()` calls
  `reset_stmt_handle(... RESET_STORE_RESULT | RESET_CLEAR_ERROR)` before every
  execution.
- `mariadb/libmysqld/libmysql.c::mysql_stmt_reset()` performs a broader
  server-side and buffer reset. MariaDB's own client tests re-execute a
  prepared result statement after fetching to `MYSQL_NO_DATA` without an
  intervening `mysql_stmt_reset()`.

## Design

- Treat any executed statement with `done == true` and `result_active == false`
  as successfully drained for reset purposes.
- Keep the existing active-result behavior: if the user resets before draining
  all rows, call `mysql_stmt_free_result()` and then `mysql_stmt_reset()` before
  reuse.
- Keep failed, partially executed, or otherwise not-done statements on the full
  MariaDB reset path.
- Preserve SQLite-style MyLite binding semantics: reset keeps current
  parameter bindings, and `mylite_clear_bindings()` remains the explicit
  binding release operation.

## Affected Subsystems

- `libmylite` prepared statement reset and re-execution behavior.
- Prepared point-select and result-producing statement performance.

## Compatibility Impact

No public API behavior change. Applications can still reset active result sets
before draining them, reset fully drained result statements, and reuse bound
parameters. The fast path relies on MariaDB's own execute path resetting stored
result state before re-execution.

## Single-File And Embedded Lifecycle Impact

No durable file, storage-engine, journal, lock, recovery, or companion-file
lifecycle change.

## Public API And File-Format Impact

No public API signature or durable file-format change.

## Storage-Engine Routing Impact

No routing change. The optimization applies to prepared statements over any
supported routed or non-storage SQL result.

## Binary-Size And Dependency Impact

Small first-party C++ branch change and one statement test. No new dependency
and no meaningful binary-size impact expected.

## Tests And Verification

- Add prepared statement coverage for a result statement that first drains with
  zero rows, resets, reuses its existing bound value, rebinds only the predicate
  parameter, and then returns a row.
- Keep existing reset-before-drain and variable-result reset coverage passing.
- Build `mylite_embedded_statement_test`, `mylite_embedded_storage_engine_test`,
  and `mylite_perf_baseline` with the `storage-smoke-dev` preset.
- Run `build/storage-smoke-dev/packages/libmylite/mylite_embedded_statement_test`.
- Run focused and full storage-smoke CTest.
- Run prepared point-select baseline samples.
- Run `git diff --check` and `git clang-format --diff` on touched C/C++ files.

## Acceptance Criteria

- Fully drained result-producing prepared statements reset and re-execute
  correctly without the explicit `mysql_stmt_reset()` call from `mylite_reset()`.
- Result statements reset before drain still use the full MariaDB reset path.
- Existing prepared statement, storage-engine, and storage-smoke tests pass.
- Benchmark evidence records the local prepared point-select impact.

## Verification Evidence

- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_statement_test mylite_embedded_storage_engine_test
  mylite_perf_baseline` passed.
- `build/storage-smoke-dev/packages/libmylite/mylite_embedded_statement_test`
  passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'libmylite.embedded-statement|libmylite.embedded-storage-engine'
  --output-on-failure` passed, 2/2 tests.
- `ctest --preset storage-smoke-dev --output-on-failure` passed, 10/10 tests.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=all 10000
  10000` recorded prepared primary-key point selects at `9.751 us/op` after
  the change, versus `9.640 us/op` in the immediately preceding sample. No
  latency claim is made from this noisy local result; the accepted effect is
  narrower prepared reset call-path cleanup.
- `git diff --check` and `git clang-format --diff` passed.

## Risks And Unresolved Questions

- This is intentionally limited to fully drained statements. Skipping reset for
  active cursors or failed executions would risk stale server-side cursor state.
- MariaDB's execute path currently clears stored-result state before every
  execution on the selected base. A future MariaDB rebase should re-check
  `mysql_stmt_execute()` before keeping this fast path.

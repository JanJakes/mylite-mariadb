# Prepared Non-Result Reset Fast Path

## Problem

The update benchmark's prepared path executes a single non-result prepared
`UPDATE` statement in a tight reset/rebind/re-execute loop. After each
successful non-result execution, `mylite_reset()` calls MariaDB
`mysql_stmt_reset()` even though there is no active result set to abandon and
the next execution can reuse the prepared statement handle.

That reset call is part of the hot prepared DML loop and is not needed for the
SQLite-style public reset semantics MyLite exposes for successful non-result
statements.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc::mylite_reset()` frees active result
  sets, then calls `mysql_stmt_reset()` for every executed statement.
- `execute_statement()` marks statements with no result columns as `done` after
  reading affected rows and warnings.
- The update performance baseline reuses one prepared non-result statement and
  calls `mylite_reset()` after every successful `MYLITE_DONE`.

## Design

- Keep full MariaDB `mysql_stmt_reset()` behavior for active or reusable result
  statements, including result statements reset before all rows are drained.
- Skip `mysql_stmt_reset()` when the previous execution successfully completed a
  statement with no result columns.
- Preserve public MyLite reset semantics by still clearing `executed`, `done`,
  and current-row state, preserving bound parameters, and returning `MYLITE_OK`.
- Add prepared DML reset-reuse coverage that rebinds and re-executes the same
  successful `UPDATE` statement.

## Affected Subsystems

- `libmylite` prepared statement reset/re-execution behavior.
- Storage-smoke prepared update performance baseline.

## Compatibility Impact

Public `mylite_reset()` behavior remains SQLite-style: reset makes the
statement ready to execute again and keeps existing bindings until
`mylite_clear_bindings()`. MySQL/MariaDB result-statement reset behavior is
preserved for active result sets.

## Single-File And Lifecycle Impact

No durable file-format, companion-file, lock, transaction, journal, recovery, or
flush lifecycle change.

## Public API And File-Format Impact

No public API signature or file-format change.

## Binary-Size And Dependency Impact

Small first-party C++ fast path. No new dependency.

## Tests And Verification

- Rebuild `mylite_embedded_statement_test`, `mylite_embedded_storage_engine_test`,
  and `mylite_perf_baseline` with the `storage-smoke-dev` preset.
- Run `build/storage-smoke-dev/packages/libmylite/mylite_embedded_statement_test`.
- Run `ctest --test-dir build/storage-smoke-dev --output-on-failure`.
- Run `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=updates 1000
  1000000`.
- Run a sampled one-million-update benchmark with macOS `sample` and confirm
  the prepared update loop no longer pays a visible `mysql_stmt_reset()` frame
  after successful non-result executions.
- Run `git diff --check` and `git clang-format --diff` on touched C/C++ files.

## Acceptance Criteria

- Successful non-result prepared statements can be reset, rebound, and
  re-executed without calling `mysql_stmt_reset()`.
- Active result statements still free/reset through MariaDB before reuse.
- Existing prepared statement, storage-engine, and storage-smoke tests remain
  green.
- Benchmark/profile evidence records the prepared update latency impact.

## Verification Evidence

- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-updates
  1000 1000000` measured one focused run at `4.472 us/op` prepared primary-key
  updates after the fast path.
- A same-harness A/B run with only this fast path reverted measured
  `4.580 us/op` prepared primary-key updates on this machine. The delta is
  small enough to treat as local evidence, not a portable threshold.
- A five-second macOS `sample` run over the focused prepared-updates phase
  recorded `mylite_reset()` samples but no `mysql_stmt_reset()` frame in the
  successful non-result prepared update loop.

## Risks And Open Questions

- This relies on MariaDB prepared statements allowing re-execution of a
  successful non-result statement after bindings are updated without an
  intervening `mysql_stmt_reset()`. The embedded statement regression test and
  storage-smoke suite cover that behavior for MyLite's current MariaDB base.

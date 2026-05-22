# Prepared Update Component Benchmark

## Problem

Prepared primary-key updates are the next measured hot path after routed
primary-key point reads. The existing `prepared-updates` benchmark reports one
combined timing for binding the primary-key parameter, executing MariaDB's
prepared UPDATE path, MyLite handler row mutation, and resetting the statement
for reuse.

That coarse number is not enough to choose the next optimization safely. A
diagnostic phase should expose whether the cost is in parameter binding,
`mylite_step()` execution, or `mylite_reset()` reuse.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_prepare.cc::Prepared_statement::execute_loop()` sets
  parameters and calls `Prepared_statement::execute()` for each prepared
  execution.
- `mariadb/sql/sql_update.cc` routes non-batched single-table updates through
  `handler::ha_update_row()`.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::update_row()` prepares
  index-entry changes, duplicate-key and foreign-key checks, row payload bytes,
  and then calls the MyLite storage update path.
- `packages/libmylite/src/database.cc::mylite_step()` calls
  `execute_statement()` on the first step after reset. Prepared non-result
  statements execute through the MyLite checkpoint and MariaDB
  `mysql_stmt_execute()` path.
- `packages/libmylite/src/database.cc::mylite_reset()` keeps successful
  non-result prepared statements reusable without a redundant MariaDB reset.

## Design

- Add an opt-in `prepared-update-components` benchmark phase.
- Keep the SQL shape identical to the existing prepared update phase:
  `UPDATE perf_rows SET value = value + 1 WHERE id = ?`.
- Run the same loop inside one explicit transaction.
- Time three components separately:
  - scalar parameter bind,
  - `mylite_step()` execution through MariaDB and MyLite storage,
  - `mylite_reset()` statement reuse.
- Print each component as an ordinary benchmark metric so local thresholds can
  be applied while investigating regressions.

## Compatibility Impact

No SQL, storage semantics, or public C API behavior changes. This is local
performance tooling.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, catalog, or companion-file lifecycle
change.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing change. The phase uses the same routed `ENGINE=InnoDB` table as the
existing prepared update benchmark.

## Binary-Size And Dependency Impact

The benchmark binary gains one focused phase. No library binary-size or
dependency impact.

## Tests And Verification

- Build `mylite_perf_baseline`.
- Run the new `prepared-update-components` phase.
- Run the existing prepared update phase to keep the coarse metric available.
- Run `git diff --check` and clang-format diff checks.

## Verification Evidence

- `git diff --check`
- `git clang-format --diff -- tools/mylite_perf_baseline.c`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`
- `tools/mylite-perf-baseline --phase=prepared-update-components 1000 10000`
  - Bind component: `0.023 us/op`.
  - Step component: `3.550 us/op`.
  - Reset component: `0.028 us/op`.
- `tools/mylite-perf-baseline --phase=prepared-updates 1000 10000`
  - Prepared primary-key updates: `3.592 us/op`.

## Acceptance Criteria

- `tools/mylite-perf-baseline --phase=prepared-update-components` is accepted
  by argument parsing.
- The phase prints separate bind, step, and reset component metrics.
- The existing prepared update phase remains available and unchanged.
- Local verification records component timings for the current write hot path.

## Risks And Unresolved Questions

- The component phase calls `clock_gettime()` several times per iteration, so
  it is diagnostic rather than a replacement for the coarse prepared update
  benchmark.

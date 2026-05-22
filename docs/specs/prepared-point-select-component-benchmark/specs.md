# Prepared Point-Select Component Benchmark

## Problem

Prepared primary-key point selects are now fast enough that coarse whole-loop
timings hide where the remaining microseconds are spent. Existing phases
separate routed prepared SELECTs from storage-level row lookups, but they do not
split one prepared SELECT iteration into bind, first row fetch, result drain,
and reset components.

Without that split, the next optimization target is easy to guess wrong. A
small benchmark-only phase should expose whether the hot path is dominated by
parameter binding, MariaDB execution/fetch, the `MYSQL_NO_DATA` drain step, or
`mylite_reset()`.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc::mylite_step()` executes a prepared
  statement on the first call and fetches subsequent rows on later calls.
- `packages/libmylite/src/database.cc::mylite_reset()` keeps fully drained
  prepared statements reusable without a redundant MariaDB reset.
- `tools/mylite_perf_baseline.c::benchmark_prepared_point_selects()` currently
  reports one combined timing for bind, first row, column read, drain, and
  reset.

## Design

- Add an opt-in `prepared-pk-select-components` benchmark phase.
- Keep the SQL shape identical to the existing prepared primary-key point-select
  phase: `SELECT value FROM perf_rows WHERE id = ?`.
- Time four components separately inside the same loop:
  - scalar parameter bind,
  - first `mylite_step()` plus column type/value read,
  - second `mylite_step()` that drains to `MYLITE_DONE`,
  - `mylite_reset()`.
- Print each component as an ordinary benchmark metric so local thresholds can
  be applied when investigating regressions.

## Compatibility Impact

No SQL or public C API behavior changes. This is local performance tooling.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, catalog, or companion-file lifecycle
change.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing change. The phase uses the same routed `ENGINE=InnoDB` table as the
existing prepared point-select benchmark.

## Binary-Size And Dependency Impact

The benchmark binary gains one focused phase. No library binary-size or
dependency impact.

## Tests And Verification

- Build `mylite_perf_baseline`.
- Run the new `prepared-pk-select-components` phase.
- Run the existing prepared primary-key point-select phase to keep the coarse
  metric available.
- Run `git diff --check` and clang-format diff checks.

## Verification Evidence

- `git diff --check`
- `git clang-format --diff -- tools/mylite_perf_baseline.c`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`
- `tools/mylite-perf-baseline --phase=prepared-pk-select-components 1000 100000`
  - Bind component: `0.022 us/op`.
  - Row component: `2.186 us/op`.
  - Done component: `0.068 us/op`.
  - Reset component: `0.022 us/op`.
- `tools/mylite-perf-baseline --phase=prepared-pk-selects 1000 100000`
  - Prepared primary-key point selects: `2.642 us/op`.

## Acceptance Criteria

- `tools/mylite-perf-baseline --phase=prepared-pk-select-components` is accepted
  by argument parsing.
- The phase prints separate bind, row, done, and reset component metrics.
- The existing prepared point-select phase remains unchanged.
- Local verification records component timings for the current hot path.

## Risks And Unresolved Questions

- The component phase calls `clock_gettime()` several times per iteration, so
  it is diagnostic rather than a replacement for the coarse point-select
  benchmark.

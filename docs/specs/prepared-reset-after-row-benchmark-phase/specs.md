# Prepared Reset-After-Row Benchmark Phase

## Problem

The existing prepared primary-key point-select benchmark drains each result set
by stepping from `MYLITE_ROW` to `MYLITE_DONE` before calling `mylite_reset()`.
SQLite-style point lookup loops often reset immediately after consuming the
expected row. MyLite has coverage for reset-before-drain behavior, but the
performance harness does not measure that workload separately.

Before changing reset-before-drain internals, the benchmark needs an explicit
phase and metric for prepared point selects that reset after the first row.

## Source Findings

- `tools/mylite_perf_baseline.c::benchmark_prepared_point_selects()` currently
  binds one primary-key parameter, steps to `MYLITE_ROW`, reads the value,
  steps again to `MYLITE_DONE`, then resets.
- `packages/libmylite/tests/embedded_statement_test.c` already covers
  reset-before-drain result reuse for correctness.
- The benchmark phase parser and threshold system already support focused
  point-select phases and per-metric `--max-us` gates.

## Design

- Add phase `prepared-pk-select-reset-after-row`.
- Add metric `prepared-pk-select-reset-after-row`.
- Add a benchmark function that performs the same prepared primary-key lookup
  as `prepared-pk-selects`, but calls `mylite_reset()` immediately after reading
  the row.
- Include the new benchmark in `--phase=point-selects` and `--phase=all`.
- Keep the existing drained-result `prepared-pk-selects` phase unchanged.

## Compatibility Impact

No runtime behavior change. This is measurement-only.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, catalog, or companion-file lifecycle
change.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing change. The benchmark continues to use `ENGINE=InnoDB` routed through
the MyLite storage engine.

## Binary-Size And Dependency Impact

Benchmark-only code. No library dependency or shipped runtime-size impact.

## Tests And Verification

- Build `mylite_perf_baseline`.
- Run `--phase=prepared-pk-select-reset-after-row`.
- Run `--phase=point-selects` to prove the focused grouped phase includes the
  new metric.
- Run an impossible `--max-us=prepared-pk-select-reset-after-row:0.001` gate and
  verify it fails.
- Run `git diff --check` and C formatting checks.

## Verification Evidence

- `git diff --check`
- `git clang-format --diff -- tools/mylite_perf_baseline.c`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-select-reset-after-row 10000 100000`
  - Prepared primary-key point selects reset after row: `9.775 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=point-selects 1000 1000`
  - Direct primary-key point selects: `16.216 us/op`.
  - Prepared primary-key point selects: `8.147 us/op`.
  - Prepared primary-key point selects reset after row: `7.886 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-select-reset-after-row --max-us=prepared-pk-select-reset-after-row:0.001 100 10`
  - Failed as expected with the metric threshold diagnostic.

## Acceptance Criteria

- The benchmark accepts `--phase=prepared-pk-select-reset-after-row`.
- The metric can be targeted by `--max-us`.
- The benchmark validates the same checksum as the drained prepared point-select
  loop.
- README usage lists the new phase.

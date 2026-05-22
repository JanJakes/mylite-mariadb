# Prepared Assignment Update Component Benchmark

## Problem

The prepared-update component benchmark currently measures
`UPDATE perf_rows SET value = value + 1 WHERE id = ?`. That expression update
is the right hot-path benchmark for repeated row mutation, but it always needs
normal value-expression setup before larger prepared-DML reuse work can remove
`JOIN::prepare()` and table-open overhead.

The simple value setup-elision slice covers a different shape:
`UPDATE ... SET value = ? WHERE id = ?`. Without a dedicated benchmark phase,
that fast path has no focused measurement and can be confused with the
expression-update component.

## Source Findings

- `tools/mylite_perf_baseline.c::benchmark_prepared_update_components()` uses
  `SET value = value + 1 WHERE id = ?`.
- `mariadb/sql/sql_update.cc::mylite_prepare_single_update_values()` can skip
  `setup_fields()` only when every assigned value is an evaluable basic
  constant.
- A bound scalar assignment becomes an evaluable basic constant after binding,
  while `value + 1` remains an expression that must use the normal setup path.

## Design

- Add a focused `prepared-assignment-update-components` benchmark phase.
- Use `UPDATE perf_rows SET value = ? WHERE id = ?`.
- Split bind, step, and reset timings with separate threshold metric names.
- Keep the existing `prepared-update-components` phase unchanged as the
  expression-update benchmark.

## Compatibility Impact

No product behavior change. This is measurement-only.

## Single-File And Lifecycle Impact

No storage format, sidecar, lock, journal, or lifecycle change.

## Public API And File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Test Plan

- Build `mylite_perf_baseline`.
- Run `mylite_perf_baseline --phase=prepared-assignment-update-components`.
- Run the existing expression phase to keep the comparison explicit.
- Run the storage-smoke test preset because the benchmark tool is part of the
  default developer verification loop.

## Verification

- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`
  passed.
- `ctest --preset storage-smoke-dev --output-on-failure` passed.
- Isolated `prepared-assignment-update-components 10000 1000000` runs measured
  the step component at 1.622-1.629 us/op.
- A sampled assignment run showed `mylite_prepare_single_update_values()` in
  the assignment phase going directly to assignability checks; value-list
  `setup_fields()` remained in `JOIN::prepare()` and table-open paths, not in
  the MyLite single-update value setup helper.

## Acceptance Criteria

- The new phase reports bind, step, reset, and ordered-scan rows.
- Existing phase names and threshold metric names continue to work.
- The expression and simple-assignment update phases remain separate in docs
  and output.

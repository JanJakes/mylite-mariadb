# Prepared Insert Component Benchmark

## Problem Statement

Prepared inserts still measure around `6.6 us/op` locally in the existing
aggregate `prepared-updates` and update-component phases. That aggregate row
does not split bind, execute, and reset costs, while prepared read and update
paths already have component phases that identify the actual hot segment before
optimization work.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/mylite_perf_baseline.c::benchmark_prepared_insert_rows()` reports one
  aggregate `prepared-inserts` metric for a prepared
  `INSERT INTO ... VALUES (?, ?, ?)` loop.
- `tools/mylite_perf_baseline.c::benchmark_prepared_assignment_update_components()`
  and `benchmark_prepared_row_only_update_components_with_mode()` already split
  analogous prepared row-DML loops into bind, step, and reset timings.
- `packages/libmylite/src/database.cc::mylite_step()` and
  `execute_simple_result_statement()` route prepared non-result statements
  through the MariaDB embedded execute path before MyLite observes affected
  rows and status fields.

## Scope

- Add a `prepared-insert-components` performance phase.
- Split prepared insert execution into:
  - `prepared-insert-bind`;
  - `prepared-insert-step`;
  - `prepared-insert-reset`.
- Use the same routed `ENGINE=InnoDB` MyLite storage path and the same
  representative `(id, value, pad)` insert shape as the existing aggregate
  prepared insert benchmark.
- Update the README performance command list and roadmap performance notes.

## Non-Goals

- No prepared insert shortcut in this slice.
- No public API, SQL behavior, storage format, or handler behavior change.
- No benchmark threshold. Timings remain local and descriptive.

## Design

The new phase creates an independent `perf_prepared_component_rows` table,
opens a transaction, prepares the same three-parameter insert shape, and inserts
`iterations` unique rows while timing bind, step, and reset separately. The
phase verifies the final row count before commit cleanup finishes.

The existing aggregate prepared insert benchmark remains unchanged and stays in
the default `all` and update-oriented phase flow. The new component phase is
opt-in so ordinary benchmark output does not grow unless requested.

## Compatibility Impact

No SQL-visible behavior change. The new code is a benchmark-only caller of the
existing public `libmylite` prepared-statement API.

## DDL Metadata Routing Impact

No metadata routing behavior change. The benchmark uses an ordinary routed
table definition already covered by existing storage-smoke tests.

## Single-File And Recovery Impact

No durable file-format, journal, recovery, lock, or companion-file lifecycle
change.

## Public API, File Format, And Routing Impact

No public C API, file-format, storage-engine routing, or wire-protocol change.

## Build, Size, And Dependencies

No dependency or license change. Binary impact is limited to the local
performance tool.

## Test Plan

- Build `mylite_perf_baseline`.
- Run `prepared-insert-components` with a focused local sample.
- Run an existing prepared update component phase to ensure option parsing and
  metric additions did not break existing phases.
- Run `git diff --check`.
- Run `git clang-format --diff` on `tools/mylite_perf_baseline.c`.

## Acceptance Criteria

- The new phase prints bind, step, and reset component rows.
- The phase inserts and verifies the expected number of rows.
- Existing performance phases continue to parse and run.
- README and roadmap performance notes mention the new phase.

## Risks And Open Questions

- The insert component phase writes one row per iteration, so very large
  iteration counts create correspondingly large temporary benchmark files. That
  is intentional for an insert benchmark, but ordinary quick samples should use
  bounded iteration counts.

## Verification Results

Local environment: macOS worktree, `storage-smoke-dev` preset.

- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`
  passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-insert-components 1000 10000` passed and reported:
  - `prepared insert bind component`: `0.132 us/op`;
  - `prepared insert step component`: `5.064 us/op`;
  - `prepared insert reset component`: `0.020 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-assignment-update-components 1000 1000` passed as an
  existing phase smoke check.
- `git diff --check` passed.
- `git clang-format --diff HEAD -- tools/mylite_perf_baseline.c` passed.

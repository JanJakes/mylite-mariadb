# Point-Select Benchmark Phases

## Problem

The local all-phase performance baseline is now too broad for point-read work:
at 10k rows and 10k iterations, secondary exact-select phases dominate runtime
by materializing 10 million result rows. That makes prepared primary-key point
select work slow to iterate on, even though the benchmark already has separate
metrics for direct and prepared primary-key point selects.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), though this slice is limited
  to first-party benchmark tooling.
- `tools/mylite_perf_baseline.c` supports focused update phases:
  `updates`, `direct-updates`, and `prepared-updates`.
- The tool already implements `benchmark_point_selects()` and
  `benchmark_prepared_point_selects()`, but they can currently run only under
  `--phase=all`.
- `README.md` documents benchmark phase usage and opt-in machine-local
  thresholds.

## Design

- Add focused phases:
  - `point-selects` for direct and prepared primary-key point selects;
  - `direct-pk-selects` for direct primary-key point selects only;
  - `prepared-pk-selects` for prepared primary-key point selects only.
- Keep database setup and direct row insertion because point selects need the
  `perf_rows` table.
- Skip prepared insert, secondary read, update, and ordered-scan phases for
  the new point-select phases.
- Keep metric names and threshold handling unchanged; existing
  `direct-pk-selects` and `prepared-pk-selects` metric thresholds apply.

## Compatibility Impact

No product behavior change. This is local benchmark tooling only.

## Single-File And Embedded Lifecycle Impact

No durable file-format or lifecycle change beyond the temporary benchmark
database the tool already creates and removes.

## Public API And File-Format Impact

No public API or durable file-format change.

## Binary-Size And Dependency Impact

Tool-only C change. No dependency or runtime library-size impact.

## Tests And Verification

- Build `mylite_perf_baseline` with the `storage-smoke-dev` preset.
- Run `--phase=point-selects`, `--phase=direct-pk-selects`, and
  `--phase=prepared-pk-selects` smoke samples.
- Run an accepted threshold for `prepared-pk-selects`.
- Run an intentionally impossible threshold and confirm the tool exits
  nonzero with the threshold diagnostic.
- Run `git diff --check` and `git clang-format --diff` on the touched C file.

## Acceptance Criteria

- Focused point-select phases run only the requested point-select metrics after
  setup and direct row insertion.
- Existing update and all phases keep their previous behavior.
- Thresholds work with the new focused phases.

## Verification Evidence

- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`
  passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=point-selects
  1000 1000` passed and printed direct plus prepared primary-key point-select
  metrics without secondary or update phases.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=direct-pk-selects
  1000 1000` passed and printed only the direct primary-key point-select metric
  after setup and direct inserts.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-pk-selects --max-us=prepared-pk-selects:1000 1000 1000`
  passed and applied the existing prepared point-select threshold metric.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-pk-selects --max-us=prepared-pk-selects:0.001 100 10`
  exited nonzero with the expected threshold diagnostic.
- `git diff --check` and `git clang-format --diff` passed.

## Risks And Unresolved Questions

- The focused phases are measurement aids, not product performance work.
  Future point-read slices still need code changes in `libmylite`, the handler,
  or storage.

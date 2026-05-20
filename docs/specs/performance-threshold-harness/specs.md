# Performance Threshold Harness

## Problem

MyLite now has a local routed-storage performance baseline, but the tool only
prints timings. That makes performance slices hard to gate: reviewers can see a
number, but cannot ask the benchmark itself to fail when a selected hot path
crosses an agreed local threshold.

The threshold support must stay opt-in because the benchmark is
machine-dependent. Default `tools/mylite-perf-baseline` runs should remain
descriptive and should not fail solely because a developer is on slower
hardware.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/mylite_perf_baseline.c` prints a Markdown timing table through
  `print_result()` and exits nonzero only for functional benchmark failures.
- `docs/ROADMAP.md` keeps benchmark thresholds as remaining row/index storage
  work, and `docs/architecture/storage.md` describes the perf harness as local
  evidence for storage work.
- Fresh local sampling on 2026-05-21 shows the prepared-update benchmark at
  roughly `3.3 us/op` over a long 1,000,000-iteration run, with remaining hot
  samples dominated by MariaDB prepared DML execution and range planning rather
  than MyLite storage page writes.

## Design

- Add an opt-in `--max-us=<metric>:<value>` command-line option to
  `tools/mylite-perf-baseline`.
- Keep metric names stable and short, for example `prepared-updates`,
  `direct-updates`, `direct-pk-selects`, and `ordered-scan`.
- Record thresholds in the parsed benchmark config as microseconds per
  operation. A value of `0` remains invalid so omitted metrics are the only
  disabled state.
- Continue printing each timing row before checking its threshold. If the
  metric exceeds the configured maximum, print a concise diagnostic to stderr
  and return a functional failure from the benchmark.
- Permit multiple `--max-us` options in one run.

## Compatibility Impact

No SQL, storage, or public `libmylite` API behavior changes. The new option
affects only the standalone local benchmark tool.

## Single-File And Lifecycle Impact

No file-format or lifecycle change. Benchmark databases remain temporary unless
`MYLITE_PERF_KEEP_ROOT=1` is set.

## Binary-Size And Dependency Impact

No new dependency. The added code is limited to the benchmark executable.

## Tests And Verification

- Run a normal prepared-update benchmark and confirm it still prints the
  existing table shape.
- Run the same benchmark with a generous prepared-update threshold and confirm
  it succeeds.
- Run the benchmark with an intentionally impossible prepared-update threshold
  and confirm it exits nonzero with a threshold diagnostic.
- Run storage-smoke build/test coverage plus formatting checks.

## Acceptance Criteria

- Default benchmark behavior stays unchanged.
- `--max-us=<metric>:<value>` accepts known metric names and rejects unknown or
  malformed threshold arguments.
- Multiple threshold options can be supplied.
- Exceeded thresholds fail the benchmark after printing the measured row.

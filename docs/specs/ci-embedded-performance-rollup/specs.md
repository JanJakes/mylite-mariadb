# CI Embedded Performance Rollup

## Problem

The embedded performance probe already emits the data needed to answer
startup, read, insert, and ownerless bulk-write questions, but CI only uploads
the raw logs as artifacts. When a run looks slow, reviewers must download and
grep several large logs before seeing the key ratios. That obscures whether
the current bottleneck is PHPUnit orchestration, process startup, read
overhead, or ownerless write-path engine work.

The latest production samples show the important current distinction:
WordPress PHPUnit visibility has been split into setup and test-only shards,
while ownerless engine throughput still has a write-path gap. A local
production 100-row bulk sample on this branch reported ownerless bulk rows at
about `0.326x` ordinary, and a stats-enabled 100-row attribution sample placed
most of the remaining ownerless-minus-ordinary cost in non-empty-table row
insert and undo-report mini-transaction work. The rollup must surface those
numbers without changing storage semantics.

## Source Findings

- `.github/workflows/ci.yml` runs four embedded performance probe shapes and
  writes raw logs to `build/embedded-performance-reports`.
- `packages/libmylite/tests/embedded_performance_probe.c` already emits compact
  `mylite_perf_summary_*` rows for warm open/close, point-select ratios,
  autocommit insert ratios, bulk rows throughput, ownerless bulk `mysql_query()`
  time, and remaining-statement undo-report MTR cost.
- `tools/check-ci-production-builds` already audits the embedded performance
  probe steps so timing samples come from production build directories.

## Design

Add `tools/embedded-performance-rollup`, a dependency-free shell tool that
reads `*.log` files from an embedded performance report directory and emits one
Markdown table with the key metrics:

- ordinary and ownerless warm open/close time;
- direct and prepared point-select ratios;
- ownerless autocommit insert ratio;
- ownerless bulk rows ratio and rows per second;
- ownerless bulk `mysql_query()` milliseconds per statement;
- remaining non-empty-table undo-report MTR milliseconds per statement.

Wire the tool into CI after the embedded performance probes and before the
artifact upload. CI writes the rollup to
`build/embedded-performance-reports/summary.md` and appends it to the GitHub
step summary when available. Extend `tools/check-ci-production-builds` so the
summary step cannot be removed without failing the existing production-build
audit.

## Compatibility Impact

None. This is CI and tooling observability only. It does not change SQL
behavior, public APIs, native InnoDB storage, ownerless locking, WAL/checkpoint
formats, recovery, directory layout, or build presets.

## Test And Verification Plan

- Add a synthetic shell self-test for `tools/embedded-performance-rollup`.
- Register that self-test in `tools/CMakeLists.txt`.
- Run the new CTest selector and the existing production-build audit.
- Run `bash -n` over the new scripts.
- Run format and diff whitespace checks.

## Acceptance Criteria

- CI surfaces embedded startup/read/write ratios directly in the GitHub step
  summary.
- The uploaded embedded performance artifact includes `summary.md`.
- Missing metrics render as `n/a` rather than hiding a report.
- Production-build guard coverage fails if the rollup step is removed.
- Docs keep the current write-path performance target explicit and do not
  claim ownerless concurrency completion.

## Verification Results

Passed:

- `bash -n tools/embedded-performance-rollup
  tools/embedded-performance-rollup-test tools/check-ci-production-builds`
- `tools/embedded-performance-rollup-test`
- `tools/check-ci-production-builds`
- `cmake --preset prod`
- `ctest --preset prod -R
  '^tools\.(embedded-performance-rollup|ci-production-builds|wordpress-phpunit-(timing-rollup|append-timing))$'
  --output-on-failure`
- `tools/embedded-performance-rollup --input build/perf-investigation`
- `cmake --build --preset format-check-prod`
- `git diff --check`

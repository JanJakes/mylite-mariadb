# Performance Baseline Harness

## Goal

Add a repeatable developer harness for measuring the current MyLite routed
storage path before storage-level performance work. The harness should report
simple elapsed-time baselines for file open/schema setup, direct inserts,
direct primary-key point selects, direct primary-key updates, and ordered scans
through the public `libmylite` API.

## Non-Goals

- Claim that MyLite is currently competitive with SQLite, MariaDB, or MySQL.
- Add pass/fail thresholds to CI.
- Replace compatibility tests with benchmarks.
- Benchmark every SQL shape, application workload, or durability mode.

## Source Findings

The benchmark uses first-party MyLite APIs and the existing storage-smoke
profile rather than MariaDB server internals:

- `packages/libmylite/include/mylite/mylite.h` exposes the public open,
  direct execution, prepared statement, binding, stepping, and close APIs used
  by applications.
- `tools/mylite-compat-harness` already builds the storage-smoke MariaDB
  archive with `PLUGIN_MYLITE_SE=STATIC`, which is the profile needed for
  routed MyLite storage-engine execution.
- `docs/ROADMAP.md` still tracks storage-level B-tree navigation,
  WAL/checkpoints, compaction, full isolation, and full write concurrency as
  future work. The benchmark is therefore evidence gathering, not a completion
  signal for those slices.

## Compatibility Impact

No SQL or C API compatibility behavior changes. The harness measures existing
`ENGINE=InnoDB` routing through the MyLite storage engine because application
schemas usually request InnoDB even though MyLite owns the durable storage.

## Design

- Add a buildable developer executable, `mylite_perf_baseline`, behind the
  existing embedded storage-smoke profile.
- Add `tools/mylite-perf-baseline` as the stable user-facing wrapper. It builds
  the storage-smoke archive and executable, then runs the benchmark.
- Keep benchmark sizes configurable by positional arguments:
  `tools/mylite-perf-baseline [rows] [iterations]`.
- Default to 100 rows and 100 point-operation iterations so the command remains
  practical while the storage layer is still scan-heavy.
- Print Markdown-compatible rows so results can be pasted into research notes
  without post-processing.
- Use direct SQL for the initial measured paths so the first baseline has one
  narrow interpretation. Prepared routed reads are covered separately by
  `docs/specs/prepared-routed-select-reads/`; prepared timing rows should be
  added as distinct measurements instead of mixed into the direct baseline.
- Use a temporary `.mylite` file and clean it up after the run.

## File Lifecycle

The benchmark creates one temporary `.mylite` file plus MyLite-owned runtime
companions under `/tmp/mylite-perf-baseline.*`, then removes the tree before
exit.

## Embedded Lifecycle And API

The benchmark uses only the public `libmylite` C API. It routes table DDL with
`ENGINE=InnoDB` under the storage-smoke profile to exercise the MyLite storage
engine path applications are expected to use.

## Build, Size, And Dependencies

No production dependency or default library-size change. The executable only
builds when `MYLITE_WITH_MARIADB_EMBEDDED` and `MYLITE_MARIADB_HAS_MYLITE_SE`
are true.

## Test Plan

- `tools/mylite-perf-baseline 100 100`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`
- `bash -n tools/mariadb-embedded-build tools/mylite-compat-harness tools/mylite-mtr-harness tools/mylite-perf-baseline tools/mylite-size-report`
- `/opt/homebrew/opt/llvm/bin/clang-format --dry-run --Werror tools/mylite_perf_baseline.c`
- `git diff --check`

## Acceptance Criteria

- The wrapper builds and runs the benchmark under the storage-smoke profile.
- The benchmark uses `ENGINE=InnoDB` routed to MyLite storage.
- Output includes operation counts, total milliseconds, and microseconds per
  operation.
- No benchmark result is documented as a product performance claim.

## Risks And Open Questions

- Results include MariaDB embedded SQL-layer overhead as well as MyLite storage
  overhead. That is appropriate for product-facing API measurements, but lower
  storage layers will need separate microbenchmarks when B-tree, WAL, and
  compaction work starts.
- The harness is intentionally local and machine-dependent. It should be used
  for before/after comparisons on the same machine, not cross-machine claims.

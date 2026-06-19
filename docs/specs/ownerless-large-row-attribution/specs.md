# Ownerless Large-Row Attribution Probe

## Problem

Ownerless bulk-insert performance is now the visible branch performance gap,
not PHP process startup or ordinary WordPress query execution. CI already runs
a stats-off 100-row embedded bulk probe, but the stats-enabled ownerless
attribution probe still uses the default smaller statement shape. That leaves
the 100-row path dependent on local one-off logs when deciding whether the next
optimization should target page publication, page-log append, redo-leave,
native commit-loop work, or SQL/PHP overhead.

This slice makes the 100-row ownerless attribution path visible in the same
production CI artifact as the stats-off large-row throughput probe.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `.github/workflows/ci.yml` builds the embedded job with
  `php-embedded-prod`, verifies the MariaDB embedded archive is `MinSizeRel`,
  verifies the first-party CMake cache is a release build, and runs embedded
  performance probes before embedded correctness tests.
- `.github/workflows/ci.yml` already runs a stats-off large-row probe with
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100` and
  `MYLITE_PERF_INSERT_ITERATIONS=5000`, writing
  `build/embedded-performance-reports/large-row-bulk.log`.
- `.github/workflows/ci.yml` also runs a reduced page-publish attribution
  probe with `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`, but that probe does
  not set the 100-row bulk statement shape.
- `tools/check-ci-production-builds` is the workflow guard that rejects
  developer presets, missing production cache checks, merged WordPress build
  and test phases, and missing embedded performance report uploads.
- `packages/libmylite/tests/embedded_performance_probe.c` already accepts the
  environment knobs needed for this slice:
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT`,
  `MYLITE_PERF_INSERT_ITERATIONS`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS`.
- Local production 100-row page-publish attribution on this branch showed the
  bulk path spending about `4.044 ms/statement` in ownerless `mysql_query()`,
  with about `1.263 ms/statement` in page-write commit-log work,
  `0.713 ms/statement` in redo-leave work, and `181.500` redo-leave log-write
  calls per statement. The matching append-only attribution sample showed
  about `0.139 ms/statement` in page-log append and about
  `0.023 ms/statement` in history-proof pair append, so the 100-row path needs
  CI-visible native page-write/redo attribution before another optimization.

## Design

Add a production-guarded CI step named
`Run embedded large-row ownerless attribution probe` immediately after the
stats-off large-row bulk probe. The step uses:

- `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
- `MYLITE_PERF_SELECT_ITERATIONS=20`,
- `MYLITE_PERF_INSERT_ITERATIONS=1000`,
- `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100`,
- `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`.

The 1000 inserted rows preserve 10 measured 100-row statements, enough to
publish per-statement attribution while keeping diagnostic overhead bounded.
The existing 5000-row stats-off probe remains the throughput bucket; the new
probe is for attribution only.

Write the new log to
`build/embedded-performance-reports/large-row-ownerless-attribution.log` and
upload it through the existing `embedded-performance-reports` artifact. Extend
`tools/check-ci-production-builds` so the workflow audit requires:

- the new step name,
- the production build guards inside the step,
- the 100-row and stats-enabled environment settings,
- absence of append-only stats in this page-publish attribution step,
- the expected artifact log path,
- execution before embedded non-ownerless and ownerless SQL correctness tests,
- execution before the embedded performance artifact upload.

## Affected Subsystems

- CI workflow timing and artifact publication.
- CI production-build audit tooling.
- Performance documentation and compatibility matrix diagnostics.

No MariaDB runtime code, SQL execution path, storage-engine path, or public
MyLite API changes.

## Compatibility Impact

No MySQL/MariaDB SQL behavior changes. No C API, PHP API, mysqli, metadata,
wire-protocol, or directory-lifecycle behavior changes. This is diagnostic
coverage only.

## Directory And Lifecycle Impact

The embedded performance probe continues to create and remove temporary
MyLite database directories. The new CI step does not introduce durable files
outside the existing build artifact directory.

## Native Storage Impact

No native storage format or recovery behavior changes. The probe reports
existing ownerless page publication, page-log append, redo-leave, and
commit-loop counters for the 100-row statement shape.

## Build And Performance Impact

The new CI step adds one reduced diagnostic probe after the existing 100-row
stats-off throughput probe. It runs against the already-built production
embedded target, so it does not add another build. The measured path uses 10
bulk statements and is intended to add seconds rather than minutes while
making the relevant native write-path attribution visible when CI fails later.

## Test And Verification Plan

- Run `tools/check-ci-production-builds`.
- Run the production CTest wrapper for the workflow audit.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.
- Let GitHub Actions run the full production CI and inspect the uploaded
  `embedded-performance-reports` artifact for
  `large-row-ownerless-attribution.log`.

## Acceptance Criteria

- CI publishes `large-row-ownerless-attribution.log` in
  `embedded-performance-reports`.
- The new step runs under the existing production build guards.
- The new step uses the 100-row bulk statement shape and page-publish stats.
- The stats-off large-row bulk probe remains separate and keeps 5000 inserted
  rows.
- The workflow audit fails if the step is removed, debug-built, moved after
  embedded correctness tests, moved after artifact upload, or changed to the
  append-only attribution mode.
- Documentation records that this is performance visibility, not an ownerless
  correctness or redo/checkpoint optimization.

## Risks And Unresolved Questions

- The probe does not reduce ownerless write cost; it makes the high-cost native
  path visible in CI.
- Runner storage and load remain noisy. Optimization decisions still need
  repeated production samples and correctness tests before touching redo,
  checkpoint, or native page-write release semantics.

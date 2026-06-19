# CI Large-Row Bulk Probe Window

## Problem

The embedded CI job runs a stats-off large-row bulk probe with
`MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100`, but it previously kept
`MYLITE_PERF_INSERT_ITERATIONS=500`. That produces only five 100-row bulk
statements. On fast GitHub runners the ordinary MariaDB side can complete that
small sample in a few milliseconds, making the ownerless/ordinary bulk ratio
too noisy for performance triage.

The probe needs to preserve the same 100-row statement shape while collecting
enough statements to make CI timing less dominated by timer granularity and
one-off setup effects.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not modify
  MariaDB source.
- `.github/workflows/ci.yml` runs the large-row embedded performance probe
  before embedded correctness tests and guards it with `MinSizeRel` MariaDB
  embedded and first-party `Release` build checks.
- `packages/libmylite/tests/embedded_performance_probe.c` computes the number
  of bulk statements as `insert_iterations / bulk_insert_rows_per_statement`.
- `tools/check-ci-production-builds` already requires the large-row probe to
  keep `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100` and its production
  build guards.

## Design

Raise only the CI large-row probe's insert iteration count from `500` to
`5000`.

The statement shape remains unchanged:

- 100 rows per bulk `INSERT ... VALUES` statement;
- 50 bulk statements per CI sample;
- stats-off timing path;
- same production build guards;
- same embedded performance probe binary.

Update the workflow audit so CI fails if the large-row probe shrinks back to a
five-statement sample.

## Compatibility Impact

No SQL behavior, public C API, PHP API, mysqli API, storage format,
directory-layout, or ownerless concurrency behavior changes. This is CI
measurement configuration only.

## Build And Performance Impact

The embedded CI job spends more time in the large-row probe, but the expected
increase is bounded: it still performs only 50 bulk statements and remains
before correctness tests. The benefit is a more useful production timing sample
for the ownerless default-checked 100-row bulk path.

## Test Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `tools/check-ci-production-builds`.
- Run focused production CTest coverage for the CI production-build audit.
- Run a local reduced production embedded probe with the same large-row
  configuration to confirm it completes.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Acceptance Criteria

- CI uses `MYLITE_PERF_INSERT_ITERATIONS=5000` for the large-row bulk probe.
- The audit requires the 5000-row value and keeps the 100-row statement shape.
- The probe remains production-build guarded and stats-off.

## Verification Results

Local verification completed:

```text
bash -n tools/check-ci-production-builds
tools/check-ci-production-builds
ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure
cmake --build --preset format-check-prod
git diff --check
```

A local production embedded probe with the widened CI shape completed:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=20
MYLITE_PERF_INSERT_ITERATIONS=5000
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

The run reported `mylite_perf_insert_iterations=5000`,
`mylite_perf_bulk_insert_rows_per_statement=100`, and
`mylite_perf_bulk_insert_statements=50`. The same sample reported ordinary
100-row bulk throughput at `105492.08 rows/s`, ownerless 100-row bulk
throughput at `21170.61 rows/s`, and an ownerless/ordinary bulk ratio of
`0.2007`. Treat those values as a local smoke sample; the slice goal is a less
noisy CI measurement window, not a new performance claim.

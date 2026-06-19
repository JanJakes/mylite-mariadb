# CI Large-Row Bulk Performance Probe

## Problem

The default embedded performance probe keeps
`MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=4`, which is useful for small
row-list timing but does not show the large row-list shape that exercises the
ownerless default-checked bulk-insert path. After the bounded ownerless bulk
slice, a fresh reduced probe still reported
`mylite_perf_summary_ownerless_autocommit_bulk_default_checked_bulk_starts_per_statement=0.033`
for four-row statements because only the initial empty-table statement entered
that path.

CI therefore needs a separate production timing bucket for the larger
single-owner row-list shape before broader branch/main performance claims are
made.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `.github/workflows/ci.yml` already runs `mylite_embedded_performance_probe`
  through the production `php-embedded-prod` build before embedded correctness
  tests.
- `packages/libmylite/tests/embedded_performance_probe.c` defaults bulk
  statements to four rows but accepts
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT`.
- `docs/specs/ownerless-default-checked-bulk-insert/specs.md` records that the
  ownerless default-checked bulk path is a bounded large row-list optimization,
  not a general multi-row or non-empty-table bulk rewrite.
- `tools/check-ci-production-builds` audits embedded timing steps for
  `MinSizeRel` MariaDB and `Release` MyLite build guards.

## Design

Add a separate GitHub Actions step named
`Run embedded large-row bulk performance probe` between the default stats-off
embedded probe and the stats-enabled ownerless attribution probe. The step
uses the same production build artifacts and runs:

- `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`
- `MYLITE_PERF_SELECT_ITERATIONS=20`
- `MYLITE_PERF_INSERT_ITERATIONS=500`
- `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100`

The step remains stats-off so its summary rows are comparable with the default
throughput probe, while using a row-list shape that makes
`mylite_perf_summary_*_bulk_*` output represent the large-row fast path.

Extend `tools/check-ci-production-builds` so the workflow audit requires this
step, verifies it runs before embedded correctness tests, verifies the same
production build guards, and verifies the `100` row-list setting.

## Compatibility Impact

No SQL behavior, C API behavior, PHP behavior, native storage format, page-log
format, checkpoint format, or directory lifecycle behavior changes. This is a
CI and diagnostics change only.

## Performance Impact

CI gains one bounded production performance-probe process. It keeps timing
visibility separate from correctness tests and avoids conflating the default
four-row bulk shape with the larger row-list path added for ownerless bulk
inserts.

## Test Plan

- Run `tools/check-ci-production-builds`.
- Run `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`.
- Build the production embedded performance probe.
- Run the new large-row probe command locally and confirm the summary reports
  `mylite_perf_bulk_insert_rows_per_statement=100`.
- Run formatting and whitespace checks.

## Acceptance Criteria

- CI has a visible production step for large-row bulk timing.
- The workflow audit fails if the step loses production build guards or the
  100-row row-list setting.
- The default small-row stats-off probe and stats-enabled ownerless
  attribution probe remain separate.

## Verification Results

Local verification on 2026-06-19:

- `tools/check-ci-production-builds` passed and reported
  `ci_production_build_audit_ok`.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure` passed.
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded` passed.
- `tools/require-cmake-release-build build/php-embedded-prod` passed.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe` passed.
- The new large-row stats-off probe command passed:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
  MYLITE_PERF_SELECT_ITERATIONS=20
  MYLITE_PERF_INSERT_ITERATIONS=500
  MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100
  build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`.
  It reported `mylite_perf_bulk_insert_rows_per_statement=100`,
  `mylite_perf_bulk_insert_statements=5`,
  `mylite_perf_summary_ordinary_insert_autocommit_bulk_rows_ops_per_second=90945.32`,
  `mylite_perf_summary_ownerless_insert_autocommit_bulk_rows_ops_per_second=23681.50`,
  and `mylite_perf_summary_ownerless_insert_autocommit_bulk_rows_ratio=0.2604`.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

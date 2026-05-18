# Secondary Index Performance Baseline

## Problem

The local performance baseline reports primary-key point reads and updates, but
recent storage work also changes secondary-index exact lookup behavior. Without
secondary-index benchmark rows, future B-tree and entryset work lacks direct
before/after evidence for the path it is trying to improve.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/mylite_perf_baseline.c` creates `perf_rows` with a secondary
  `KEY value_key (value)`, but only benchmarks primary-key selects, primary-key
  updates, inserts, and ordered full scans.
- `docs/specs/performance-baseline-harness/specs.md` defines the harness as
  local machine-dependent evidence, not CI pass/fail criteria.
- `docs/specs/raw-exact-index-entrysets/specs.md` notes that the current
  primary-key baseline does not isolate non-unique secondary-index exact
  lookup.

## Design

- Populate `perf_rows.value` with duplicate buckets instead of one unique value
  per row, while keeping the primary-key benchmark deterministic.
- Add direct and prepared secondary-index exact select benchmarks using
  `FORCE INDEX (value_key)` and a deterministic `ORDER BY id`, then verify that
  each iteration returns the expected bucket count and checksum.
- Keep the benchmark local and Markdown-compatible. Do not add thresholds or
  product performance claims.
- Keep row/update checks based on current values after the update benchmarks so
  checksum assertions remain deterministic.

## Compatibility Impact

No SQL behavior changes. The benchmark exercises ordinary routed InnoDB table
DDL and secondary-index equality predicates through the public `libmylite` API.

## Single-File And Lifecycle Impact

No storage format or lifecycle change. The benchmark still creates one
temporary `.mylite` file plus MyLite-owned runtime companions under the
temporary benchmark root and removes them on exit.

## Build, Size, And Dependency Impact

No production dependency or default library-size change. The executable remains
part of the opt-in storage-smoke profile.

## Tests And Verification

- Build `mylite_perf_baseline`.
- Run `tools/mylite-perf-baseline`.
- Run changed-line `clang-format` and `git diff --check`.
- A local run with the default 100 rows and 100 iterations measured
  direct/prepared secondary-index exact selects at `5095.220` / `4945.310`
  us/op, returned `1000` rows for each direct/prepared benchmark, and produced
  checksum `56000` for both paths.

## Acceptance Criteria

- Baseline output includes labelled direct and prepared secondary-index exact
  select rows.
- Secondary benchmark checksums prove rows are actually read.
- Existing primary-key benchmark rows still run and produce correct checksums.
- The docs state that results are local evidence, not thresholds.

## Risks

- `ORDER BY id` on duplicate secondary values makes the benchmark deterministic
  but includes extra SQL-layer work. That is acceptable for API-level evidence;
  storage-only microbenchmarks remain future work.
- Results are expected to remain noisy on a developer workstation.

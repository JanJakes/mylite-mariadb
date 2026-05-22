# MTR Window Function Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with accepted upstream window and
ordered-value tests:

- `main.last_value`
- `main.win_lead_lag`
- `main.win_rank`
- `main.win_sum`
- `main.win_min_max`
- `main.win_orderby`
- `main.win_empty_over`
- `main.win_avg`
- `main.win_bit`
- `main.win_nth_value`
- `main.win_ntile`
- `main.win_percent_cume`
- `main.win_std`
- `main.win_as_arg_to_aggregate_func`
- `main.win_insert_select`

This gives the trimmed embedded profile pass-gated upstream coverage for
`LAST_VALUE()`, `LEAD()` / `LAG()`, rank-style windows, aggregate windows,
window ordering, empty window frames, nth-value and ntile behavior,
percent-rank/cumulative-distribution behavior, and insert-select use of window
results.

## Non-Goals

- Add broad window-function MTR coverage.
- Normalize expected-result files for default-engine `SHOW CREATE TABLE`
  differences under the Aria-based MTR profile.
- Enable omitted native engines, Performance Schema, system log tables, or
  server-only surfaces to admit a test.
- Treat this as MyLite storage-engine routing evidence. The MTR runner remains
  MariaDB embedded baseline coverage.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/last_value.test` covers the scalar `LAST_VALUE()`
  function, including assignment side effects, mixed types, metadata-sensitive
  paths, and parse rejection for empty argument lists.
- `mariadb/mysql-test/main/win_lead_lag.test` covers `LEAD()` and `LAG()` over
  ordered and partitioned rows with positive, zero, and negative offsets.
- `mariadb/mysql-test/main/win_rank.test` covers rank-family window functions.
- `mariadb/mysql-test/main/win_sum.test`, `win_avg.test`, `win_min_max.test`,
  `win_bit.test`, and `win_std.test` cover aggregate window variants.
- `mariadb/mysql-test/main/win_orderby.test` covers ordering behavior within
  window specifications.
- `mariadb/mysql-test/main/win_empty_over.test` covers `OVER ()` shapes and
  related EXPLAIN output.
- `mariadb/mysql-test/main/win_nth_value.test` and `win_ntile.test` cover
  positional window-function behavior.
- `mariadb/mysql-test/main/win_percent_cume.test` covers percent-rank and
  cumulative-distribution functions.
- `mariadb/mysql-test/main/win_as_arg_to_aggregate_func.test` covers window
  functions used as aggregate-function arguments.
- `mariadb/mysql-test/main/win_insert_select.test` covers window expressions in
  `INSERT ... SELECT`.
- All selected tests pass under the MyLite MTR smoke profile without upstream
  test-file changes.
- Rejected nearby probes:
  - `main.win_first_last_value` and `main.win_percentile` currently mismatch
    only default-engine `SHOW CREATE TABLE` output under the Aria-based smoke
    profile, so they need a separate normalization-policy slice.
  - `main.sysdate_is_now`, `main.func_isnull`, and several scalar-function
    candidates reach omitted server utility functions such as `SLEEP()` or
    `GET_LOCK()`.
  - Broad DML/select candidates such as `main.select`, `main.insert_select`,
    `main.delete`, and `main.update` depend on disabled native engine or
    InnoDB metadata surfaces.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected window-function behavior. This is upstream MariaDB embedded baseline
coverage over the trimmed profile, not a claim that MyLite storage routing has
exhaustive window-function coverage.

## Design

- Add the selected tests to `tools/mylite-mtr-harness`'s default curated list.
- Keep the selected cases grouped near existing expression/row tests.
- Do not modify upstream MariaDB test files or expected result files.
- Keep skipped, server-surface-dependent, and result-normalization candidates
  outside the default list.

## File Lifecycle

No MyLite `.mylite` file-format or runtime lifecycle change. The tests run in
the MTR smoke work directory under `build/mariadb-mtr-smoke/mysql-test/var`.

## Embedded Lifecycle And API

No `libmylite` API change. This only expands the opt-in MariaDB embedded MTR
baseline.

## Build, Size, And Dependencies

No dependency or production binary-size change. The opt-in MTR build tree can
be reclaimed with `rm -rf build/mariadb-mtr-smoke` or `rm -rf build`.

## Test Plan

- `tools/mylite-mtr-harness list`
- `tools/mylite-mtr-harness run main.last_value main.win_lead_lag main.win_rank main.win_sum main.win_min_max main.win_orderby main.win_empty_over main.win_avg main.win_bit main.win_nth_value main.win_ntile main.win_percent_cume main.win_std main.win_as_arg_to_aggregate_func main.win_insert_select`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Verification Evidence

- `tools/mylite-mtr-harness list | wc -l`
  - `149`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `git diff --check`
- `find mariadb/mysql-test -name '*.reject' -print`
  - no reject files.
- `tools/mylite-mtr-harness run main.last_value main.win_lead_lag main.win_rank main.win_sum main.win_min_max main.win_orderby main.win_empty_over main.win_avg main.win_bit main.win_nth_value main.win_ntile main.win_percent_cume main.win_std main.win_as_arg_to_aggregate_func main.win_insert_select`
  - all 15 selected tests passed.
- `tools/mylite-mtr-harness run`
  - `mylite.bootstrap_schema` passed.
  - all 148 selected `main` tests passed.

## Acceptance Criteria

- The default MTR smoke list includes the selected window-function tests.
- All selected tests report MTR `[ pass ]` under the MyLite MTR smoke profile.
- The full curated MTR smoke list still reports pass lines for every accepted
  default test.
- No upstream MariaDB test files or result files are modified.

## Risks And Open Questions

- Some skipped window-function candidates are likely admissible after expected
  output normalization for the Aria-based smoke profile. That should be a
  separate policy slice rather than mixed with pass-gated admission.
- This is not exhaustive window-function coverage, and it does not exercise
  MyLite's file-backed routed storage path.

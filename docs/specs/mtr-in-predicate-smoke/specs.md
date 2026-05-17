# MTR IN Predicate Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream test
`main.func_in`. This adds curated embedded baseline coverage for `IN` and
`NOT IN` predicate behavior, including NULL handling, view expansion,
prepared/dynamic statements, large `IN` lists, unsigned comparisons, and
representative indexed range plans.

## Non-Goals

- Broad optimizer, cost-model, range-access, or row-estimate compatibility.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.
- Normalizing query results or plan shape beyond engine/statistics-sensitive
  `EXPLAIN rows` estimates.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/func_in.test` covers `IN` and `NOT IN` predicate
  truth tables, NULL behavior, single-value simplification, view expansion,
  large `IN` list execution, unsigned and signed comparison edge cases,
  subqueries, prepared statements, and representative `EXPLAIN` plans over
  integer, datetime, character, and decimal indexed columns.
- Under the MyLite MTR smoke profile, query results and plan shapes match the
  upstream expected output, but a small number of `EXPLAIN rows` estimates vary
  because the profile runs with Aria as the default storage engine rather than
  the upstream test's default engine assumptions.
- The test now uses narrow `--replace_column 9 #` directives on only those
  engine-statistics-sensitive `EXPLAIN` statements. The access type, key,
  key length, ref column, Extra column, and result rows remain checked.
- Nearby catch-all function tests remain outside the curated list:
  - `main.func_int` reaches embedded built-in-function resolution differences
    such as `BENCHMARK()` resolving as a missing stored function.
  - `main.func_str`, `main.func_math`, `main.func_misc`, `main.func_group`,
    and `main.func_gconcat` include broad disabled-engine, sequence,
    server-surface, or high-volume `SHOW CREATE TABLE` sections that need
    separate admission decisions.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected `IN` / `NOT IN` predicate behavior in addition to existing bootstrap,
CAST/CONVERT, CASE-family, numeric/type/date/temporal,
parser/comment/comparison, operator, scalar-function, default-expression,
weight-string,
string/format, aggregate DISTINCT, date-format, ASCII charset, KDF,
disabled-DES, and REGEXP coverage. This remains curated MariaDB embedded
baseline coverage, not broad MTR-scale comparison and not MyLite
storage-routing evidence.

## Design

- Add `main.func_in` to `tools/mylite-mtr-harness`'s default curated list.
- Normalize only the `rows` column in the five `EXPLAIN` outputs whose row
  estimates vary under the Aria-default MTR smoke profile.
- Keep result rows and non-row-estimate plan output unchanged.
- Keep broader catch-all function and optimizer tests outside the list until
  their disabled-engine, server-surface, or normalization requirements are
  independently designed.

## File Lifecycle

No MyLite `.mylite` file format or runtime lifecycle changes. The tests run
inside `build/mariadb-mtr-smoke/mysql-test/var`.

## Embedded Lifecycle And API

No `libmylite` API change. The slice only expands opt-in MariaDB embedded MTR
baseline coverage.

## Build, Size, And Dependencies

No dependency or production binary-size change. The opt-in MTR build tree can
still be reclaimed with `rm -rf build/mariadb-mtr-smoke` or `rm -rf build`.

## Test Plan

- `tools/mylite-mtr-harness list`
- `tools/mylite-mtr-harness run main.func_in`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes `main.func_in`.
- `main.func_in` passes under the MyLite MTR smoke profile.
- Only engine-statistics-sensitive `EXPLAIN rows` estimates are normalized.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- Row-estimate normalization must stay narrow. A broader optimizer-comparison
  slice should decide how much cost-model output MyLite wants to preserve or
  normalize when the storage profile differs from upstream defaults.
- The selected test still runs against MariaDB's embedded baseline, not
  MyLite's routed storage implementation.

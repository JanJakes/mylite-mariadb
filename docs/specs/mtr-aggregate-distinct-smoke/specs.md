# MTR Aggregate DISTINCT Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.count_distinct` and `main.sum_distinct`. This adds curated embedded
baseline coverage for `COUNT(DISTINCT ...)`, `SUM(DISTINCT ...)`, aggregate
NULL behavior, aggregate use across joins, views, subqueries, grouped results,
small temporary-table limits, and selected aggregate cleanup regressions.

## Non-Goals

- Broad aggregate, grouping, optimizer, or query-result MTR coverage.
- Normalizing tests that depend on disabled engines, disabled Sequence support,
  or profile-sensitive status counters.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/count_distinct.test` exercises
  `COUNT(DISTINCT ...)` across left joins, `HAVING`, user variables, empty
  inputs, views, grouped rows, temporary-table memory pressure, and regression
  cases around cleanup and null grouping.
- `mariadb/mysql-test/main/sum_distinct.test` exercises
  `SUM(DISTINCT ...)` over empty inputs, all-NULL inputs, nested subqueries,
  cross joins, grouped rows, `SQL_BUFFER_RESULT`, string-to-number conversion,
  and a derived-table empty-input regression.
- Both tests pass under the MyLite MTR smoke profile without source changes.
- Nearby aggregate/query candidates are not yet suitable for the curated list:
  `main.distinct` and `main.group_by` are skipped because the profile disables
  the Sequence engine, `main.having` and `main.func_group` require native
  MyISAM sections, `main.count_distinct2` has profile-sensitive
  `Created_tmp_disk_tables` status output, and `main.count_distinct3` needs
  `--big-test`.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected aggregate DISTINCT behavior in addition to existing bootstrap,
CAST/CONVERT, CASE-family, numeric/date, parser/comment/comparison, scalar
operator, and string/format coverage. This remains curated MariaDB embedded
baseline coverage, not broad MTR-scale comparison and not MyLite
storage-routing evidence.

## Design

Add `main.count_distinct` and `main.sum_distinct` to
`tools/mylite-mtr-harness`'s default curated list. No MariaDB test-source
normalization is needed for these two cases.

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

- `tools/mylite-mtr-harness run main.count_distinct`
- `tools/mylite-mtr-harness run main.sum_distinct`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes `main.count_distinct` and
  `main.sum_distinct`.
- Both tests pass under the MyLite MTR smoke profile.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- Broader query-shape MTR expansion needs a clear policy for tests that are
  otherwise useful but require disabled Sequence support, native-engine
  sections, status-counter normalization, or `--big-test`.

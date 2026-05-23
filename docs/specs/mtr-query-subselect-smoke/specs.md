# MTR Query And Subselect Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.update_ignore_216`, `main.subselect_nulls`, `main.subselect_extra`, and
`main.subselect_mat_analyze_json`. This adds curated embedded baseline
coverage for an `UPDATE IGNORE` subquery regression, NULL-sensitive subquery
predicates, selected optimizer subquery cases, and JSON `ANALYZE` output for
materialized subquery strategies.

## Non-Goals

- Broad DML or subquery MTR coverage.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.
- Normalizing query suites that depend on disabled native engines,
  server-side file I/O, sequence-engine virtual tables, trimmed GIS functions,
  or broader plan-output differences.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/update_ignore_216.test` covers MDEV-216 /
  Launchpad bug 976104, an `UPDATE IGNORE` multi-table statement with a
  subquery in the `WHERE` predicate.
- `mariadb/mysql-test/main/subselect_nulls.test` disables semijoin and covers
  row-valued and scalar `IN`, `EXISTS`, `IS TRUE`, `IS FALSE`, and
  `IS UNKNOWN` behavior with `NULL` values, plus MDEV-7339 and MDEV-32555
  regressions.
- `mariadb/mysql-test/main/subselect_extra.test` collects additional subquery
  coverage from upstream `explain`, `type_datetime`, `group_min_max`,
  `group_by`, and `derived_view` tests, including datetime subqueries,
  grouped min/max index plans, `IGNORE INDEX` plans, derived tables, views, and
  materialized-view `IN` subquery regressions.
- `mariadb/mysql-test/main/subselect_mat_analyze_json.test` covers
  `EXPLAIN FORMAT=JSON` and `ANALYZE FORMAT=JSON` output for complete and
  partial materialized subquery matching, `NOT IN`, grouped/order/having
  subqueries, nested `IN`, outer-join `ON` subqueries, and row-valued
  predicates.
- All selected tests pass under the MyLite MTR smoke profile without upstream
  source changes.
- Probed nearby query and subselect suites stay outside this slice:
  - `main.select_found`, `main.insert_select`, `main.derived_opt`, and
    `main.subselect_sj` require disabled native MyISAM sections.
  - `main.select_safe` needs separate plan-output normalization review.
  - `main.function_defaults`, `main.insert`, and `main.subselect3` reach
    unsupported `SELECT ... INTO OUTFILE`.
  - `main.insert_update`, `main.update`, `main.delete`,
    `main.subselect2`, `main.subselect_elimination`, and `main.subselect_sj2`
    require disabled InnoDB startup options.
  - `main.insert_returning`, `main.delete_returning`, `main.derived`,
    `main.subselect`, `main.subselect4`, `main.subselect_no_mat`,
    `main.subselect_no_semijoin`, `main.subselect_no_exists_to_in`,
    `main.subselect_exists2in`, `main.subselect_partial_match`,
    `main.subselect_mat`, and `main.subselect_no_scache` are skipped because
    they require the virtual Sequence engine, which the harness treats as no
    coverage.
  - `main.subselect_cache` reaches the trimmed GIS `POINT()` path.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected subquery, subquery `ANALYZE FORMAT=JSON`, and update-ignore behavior
in addition to existing bootstrap,
CAST/CONVERT, CASE-family, numeric/type, date/temporal, parser/comparison,
`IN` predicate, scalar operator, scalar-function, aggregate DISTINCT,
charset/collation, KDF, disabled-DES, and REGEXP coverage. This remains curated
MariaDB embedded baseline coverage, not broad MTR-scale comparison and not
MyLite storage-routing evidence.

## Design

- Add the selected query and subselect tests to `tools/mylite-mtr-harness`'s
  default curated list.
- Do not modify upstream test files for this slice.
- Keep skipped, disabled-engine, unsupported-surface, and broad normalization
  candidates outside the list.

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
- `tools/mylite-mtr-harness run main.update_ignore_216 main.subselect_nulls main.subselect_extra main.subselect_mat_analyze_json`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes the selected query and subselect tests.
- All selected tests pass under the MyLite MTR smoke profile.
- No upstream MariaDB test files are modified for this slice.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- Broader query and subquery suites need separate disabled-engine,
  unsupported-surface, virtual-engine, and normalization policies before
  admission.
- This remains MariaDB embedded baseline coverage and does not prove MyLite
  storage-routing query behavior.

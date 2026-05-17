# MTR Prepared Statement Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.prepare`, `main.ps_10nestset`, `main.ps_11bugs`,
`main.ps_max_subselect-5113`, and `main.information_schema_prepare`. This adds
curated embedded baseline coverage for SQL PREPARE/EXECUTE behavior, bound
parameters, prepared subqueries, prepared-statement regression cases, and
prepared information-schema view creation.

## Non-Goals

- Broad prepared-statement MTR coverage.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.
- Normalizing prepared-statement suites that depend on disabled native engines,
  missing system log tables, skipped embedded modes, disabled InnoDB options,
  or reprepare-count differences.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/prepare.test` covers prepared statements over
  information-schema and ordinary table UNION queries, repeated execution after
  `FLUSH TABLES` and `ALTER TABLE`, dropped-table diagnostics, unsigned
  parameter handling, immediate execution, and selected prepared subquery
  regressions.
- `mariadb/mysql-test/main/ps_10nestset.test` covers prepared inserts and
  updates against a nested-set hierarchy table, including bound string,
  decimal, and integer parameters.
- `mariadb/mysql-test/main/ps_11bugs.test` covers prepared-statement bug
  regressions for optimized WHERE conditions, many NULL bound parameters,
  prepared joins, prepared `IN` / `UNION` subqueries, undefined user variables,
  temporal parameter execution, and invalid `DEFAULT` / `IGNORE` parameter use.
- `mariadb/mysql-test/main/ps_max_subselect-5113.test` covers MDEV-5113, where
  repeated execution of a prepared statement with an `ALL` subquery previously
  produced wrong results.
- `mariadb/mysql-test/main/information_schema_prepare.test` covers MDEV-15907,
  preparing `CREATE VIEW ... AS SELECT * FROM INFORMATION_SCHEMA.TABLES`,
  flushing privileges, and executing the prepared statement.
- All selected tests pass under the MyLite MTR smoke profile without upstream
  source changes.
- Probed nearby prepared-statement suites stay outside this slice:
  - `main.ps_1general` depends on disabled native MyISAM sections.
  - `main.ps_error` is skipped for embedded server.
  - `main.ps_ddl` depends on omitted `mysql.general_log`.
  - `main.ps_ddl1` has embedded-profile reprepare-count differences that need
    separate normalization review.
  - `main.ps_missed_cmds`, `main.subselect-crash_15755`,
    `main.optimizer_crash`, `main.mrr_derived_crash_4610`, and
    `main.statistics_index_crash-7362` require disabled InnoDB startup
    options.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected prepared-statement behavior in addition to existing bootstrap,
CAST/CONVERT, CASE-family, numeric/type, date/temporal, parser/comparison,
predicate, ORDER BY, UNION, scalar-function, aggregate DISTINCT,
charset/collation, KDF, disabled-DES, and REGEXP coverage. This remains
curated MariaDB embedded baseline coverage, not broad MTR-scale comparison and
not MyLite storage-routing evidence.

## Design

- Add the selected prepared-statement tests to `tools/mylite-mtr-harness`'s
  default curated list.
- Do not modify upstream test files for this slice.
- Keep skipped, disabled-engine, disabled-InnoDB, omitted-system-table, and
  reprepare-normalization candidates outside the list.

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
- `tools/mylite-mtr-harness run main.prepare main.ps_10nestset main.ps_11bugs main.ps_max_subselect-5113 main.information_schema_prepare`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes the selected prepared-statement tests.
- All selected tests pass under the MyLite MTR smoke profile.
- No upstream MariaDB test files are modified for this slice.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- Broader prepared-statement suites need separate disabled-engine,
  disabled-InnoDB, omitted-system-table, skipped-mode, and reprepare-count
  normalization policies before admission.
- This remains MariaDB embedded baseline coverage and does not prove MyLite
  storage-routing prepared-statement behavior.

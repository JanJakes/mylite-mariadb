# MTR ORDER BY Optimizer Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.order_by_optimizer` and `main.order_by-mdev-10122`. This adds curated
embedded baseline coverage for selected `ORDER BY` optimizer diagnostics and
aggregate expressions inside ordered grouped subqueries and `UNION` inputs.

## Non-Goals

- Broad `ORDER BY`, grouping, join, or optimizer MTR coverage.
- Normalizing status-counter, plan-output, native-engine, or Sequence-engine
  dependent suites.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/order_by_optimizer.test` covers MDEV-7885,
  requiring `EXPLAIN` to show filesort when an `ORDER BY` index is ignored,
  and MDEV-8857, where `EXPLAIN` must not report an incorrect `Distinct`
  extra flag for a join-buffer plan.
- `mariadb/mysql-test/main/order_by-mdev-10122.test` covers aggregate
  functions in `ORDER BY` clauses inside grouped subqueries, ordered subquery
  wrappers, and `UNION` operands. The test asserts those forms execute instead
  of raising "Invalid use of group function".
- Both selected tests pass under the MyLite MTR smoke profile without upstream
  source changes.
- Probed nearby candidates stay outside this slice:
  - `main.order_by_sortkey` depends on trimmed status-variable output.
  - `main.order_by_pack_big` requires `--big-test`.
  - `main.order_by_limit_join`, `main.group_by_cardinality`, and
    `main.join_cache_cardinality` are not run for embedded server.
  - `main.limit`, `main.having`, `main.join_outer`, `main.join_crash`,
    `main.select`, `main.select_found`, and `main.selectivity_no_engine`
    reach disabled native MyISAM, disabled XML, or profile-specific system
    table paths.
  - `main.union`, `main.join_nested`, and `main.order_by` require the disabled
    Sequence engine.
  - InnoDB-specific ORDER/GROUP variants require disabled native InnoDB
    startup options.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected `ORDER BY` optimizer and grouped aggregate-ordering behavior in
addition to existing bootstrap, CAST/CONVERT, CASE-family, numeric/type,
temporal, parser/comment/comparison, subquery, `IN`, `REPLACE`/`RETURNING`,
prepared-statement, scalar-function, aggregate DISTINCT, charset/collation,
KDF, disabled-DES, and REGEXP coverage. This remains curated MariaDB embedded
baseline coverage, not broad MTR-scale comparison and not MyLite storage
routing evidence.

## Design

Add `main.order_by_optimizer` and `main.order_by-mdev-10122` to
`tools/mylite-mtr-harness`'s default curated list. Do not edit upstream MariaDB
test sources or expected result files.

## File Lifecycle

No MyLite `.mylite` file format or runtime lifecycle change. The tests run
inside `build/mariadb-mtr-smoke/mysql-test/var`.

## Embedded Lifecycle And API

No `libmylite` API change. The slice only expands opt-in MariaDB embedded MTR
baseline coverage.

## Build, Size, And Dependencies

No dependency or production binary-size change. The opt-in MTR build tree can
still be reclaimed with `rm -rf build/mariadb-mtr-smoke` or `rm -rf build`.

## Test Plan

- `tools/mylite-mtr-harness list`
- `tools/mylite-mtr-harness run main.order_by_optimizer main.order_by-mdev-10122`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes `main.order_by_optimizer` and
  `main.order_by-mdev-10122`.
- Both tests pass under the MyLite MTR smoke profile.
- No upstream MariaDB test files are modified.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- Broader ORDER/GROUP/JOIN MTR expansion needs explicit policy for disabled
  native engines, disabled Sequence support, trimmed status producers,
  embedded-server skips, and plan-output normalization.
- This remains MariaDB embedded baseline coverage and does not prove MyLite
  storage-routing query behavior.

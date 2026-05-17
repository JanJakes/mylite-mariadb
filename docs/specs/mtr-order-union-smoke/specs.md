# MTR Order And Union Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.order_by_zerolength-4285`, `main.order_fill_sortbuf`, and
`main.subselect_union_rand`. This adds curated embedded baseline coverage for
ORDER BY zero-length conversion regressions, create-select sorting with merge
sort pressure, and RAND/UNION subquery regressions.

## Non-Goals

- Broad ORDER BY, GROUP BY, JOIN, or UNION MTR coverage.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.
- Normalizing status-counter-sensitive or plan-output-sensitive suites.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/order_by_zerolength-4285.test` covers MDEV-4285
  and MDEV-17020 ORDER BY crash regressions involving `NOW()`, casts to
  zero-length `CHAR`, bad conversion, and `LIMIT`.
- `mariadb/mysql-test/main/order_fill_sortbuf.test` covers a `CREATE TABLE ...
  SELECT ... ORDER BY` path with enough rows and a small `sort_buffer_size` to
  force sort-buffer merge behavior.
- `mariadb/mysql-test/main/subselect_union_rand.test` covers MDEV-32397 and
  MDEV-32403 crash regressions involving RAND(), UNION, derived tables,
  subqueries, and joins.
- All selected tests pass under the MyLite MTR smoke profile without upstream
  source changes.
- Probed nearby suites stay outside this slice:
  - `main.func_group`, `main.join_crash`, and broad join/order/group suites
    contain disabled native MyISAM sections.
  - `main.group_by_null` reaches the trimmed XML `EXTRACTVALUE()` function.
  - `main.group_by_cardinality`, `main.order_by_limit_join`,
    `main.union_crash-714`, `main.union`, and `main.join_nested` are skipped
    under the embedded smoke profile or require debug/Sequence-engine support;
    the harness treats those as no coverage.
  - `main.order_by_sortkey` is status-counter-sensitive under the trimmed
    embedded profile and needs separate normalization review.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected ORDER BY and UNION/subquery behavior in addition to existing
bootstrap, CAST/CONVERT, CASE-family, numeric/type, date/temporal,
parser/comparison, predicate, scalar-function, aggregate DISTINCT,
charset/collation, KDF, disabled-DES, and REGEXP coverage. This remains
curated MariaDB embedded baseline coverage, not broad MTR-scale comparison and
not MyLite storage-routing evidence.

## Design

- Add the selected ORDER BY and UNION/subquery tests to
  `tools/mylite-mtr-harness`'s default curated list.
- Do not modify upstream test files for this slice.
- Keep skipped, disabled-engine, unsupported-surface, and status/plan
  normalization candidates outside the list.

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
- `tools/mylite-mtr-harness run main.order_by_zerolength-4285 main.order_fill_sortbuf main.subselect_union_rand`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes the selected ORDER BY and
  UNION/subquery tests.
- All selected tests pass under the MyLite MTR smoke profile.
- No upstream MariaDB test files are modified for this slice.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- Broader ORDER BY, GROUP BY, JOIN, and UNION suites need separate
  disabled-engine, virtual-engine, status-counter, plan-output, and
  unsupported-surface policies before admission.
- This remains MariaDB embedded baseline coverage and does not prove MyLite
  storage-routing query behavior.

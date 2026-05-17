# MTR Operator Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream operator tests:
`main.func_equal` and `main.func_op`. This adds curated baseline coverage for
NULL-safe equality, arithmetic operators, bitwise operators, shifts, `MOD()`,
`BIT_COUNT()`, and representative table-backed operator predicates.

## Non-Goals

- Running broad optimizer, join, or expression suites through MTR.
- Normalizing tests with profile-specific disabled functions, engines, or row
  estimates.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/func_equal.test` exercises the NULL-safe `<=>`
  operator over scalar values, nullable table columns, and quoted unsigned
  bigint predicates.
- `mariadb/mysql-test/main/func_op.test` exercises arithmetic grouping,
  modulo forms, bitwise operators, shifts, `BIT_COUNT()`, and one
  table-backed left-join regression.
- Both tests pass under the MyLite MTR smoke profile without source changes.
- Nearby candidates are not yet suitable for the curated list:
  `main.func_math` depends on GIS functions disabled by the embedded profile,
  `main.func_like` depends on XML functions, `main.func_isnull` depends on
  `GET_LOCK()`, and `main.func_in` has optimizer row-estimate result drift.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected scalar operator behavior in addition to existing bootstrap,
CAST/CONVERT, and CASE-family coverage. This remains curated MariaDB embedded
baseline coverage, not a broad MTR-scale compatibility claim.

## Design

Add `main.func_equal` and `main.func_op` to `tools/mylite-mtr-harness`'s
default curated list. No MariaDB test-source normalization is needed for these
two cases.

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

- `tools/mylite-mtr-harness run main.func_equal`
- `tools/mylite-mtr-harness run main.func_op`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes `main.func_equal` and `main.func_op`.
- Both tests pass under the MyLite MTR smoke profile.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- Other operator/function MTR suites remain blocked on disabled runtime
  surfaces or profile-specific result differences. They need separate review
  before entering the curated list.

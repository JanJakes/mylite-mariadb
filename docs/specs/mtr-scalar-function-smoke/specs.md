# MTR Scalar Function Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.func_bit`, `main.func_extract`, and `main.func_replace`. This adds
curated embedded baseline coverage for bitwise scalar functions, `EXTRACT()`
temporal field handling, and `REPLACE()` string type inference without
claiming broad MTR-scale compatibility.

## Non-Goals

- Broad scalar-function MTR coverage.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.
- Enabling tests that depend on disabled server utility functions, XML
  functions, named-lock functions, or disabled native engines.
- Normalizing result differences beyond profile-specific default-engine
  `SHOW CREATE TABLE` output.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/func_bit.test` exercises bitwise operators and
  `BIT_COUNT()` over `BIGINT`, `DOUBLE`, and `DECIMAL` values, including
  unsigned 64-bit edge values.
- `mariadb/mysql-test/main/func_extract.test` exercises `EXTRACT()` over
  `TIME`, `DATE`, `DATETIME`, string, integer, and decimal inputs, including
  interval type inference through `CREATE TABLE ... SELECT`.
- `mariadb/mysql-test/main/func_replace.test` exercises `REPLACE()` over
  unions and `CREATE TABLE ... SELECT` result type inference.
- The MyLite MTR smoke profile intentionally disables native MyISAM and runs
  with Aria as the default MTR storage engine. These selected tests otherwise
  pass with the same narrow `ENGINE=Aria` / `ENGINE=MyISAM` and
  `PAGE_CHECKSUM=1` `SHOW CREATE TABLE` normalization already used by
  existing curated MTR tests.
- Nearby candidates remain outside the curated list:
  - `main.func_int` reaches trimmed `BENCHMARK()` behavior.
  - `main.func_isnull` reaches trimmed named-lock function behavior.
  - `main.func_like` reaches trimmed XML `EXTRACTVALUE()` behavior.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected bit, extraction, and replacement scalar-function behavior in addition
to existing bootstrap, CAST/CONVERT, CASE-family, numeric/date,
parser/comment/comparison, scalar-operator, string/format, aggregate DISTINCT,
date-format, ASCII charset, KDF, and disabled-DES coverage. This remains
curated MariaDB embedded baseline coverage, not broad MTR-scale comparison and
not MyLite storage-routing evidence.

## Design

- Add `main.func_bit`, `main.func_extract`, and `main.func_replace` to
  `tools/mylite-mtr-harness`'s default curated list.
- Add profile-sensitive `--replace_result` directives immediately before the
  affected `SHOW CREATE TABLE` outputs in the selected upstream tests.
- Keep failed candidates out of the list until their disabled-surface
  dependencies are either supported or deliberately normalized by a later
  slice.

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
- `tools/mylite-mtr-harness run main.func_extract main.func_bit main.func_replace`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes `main.func_bit`, `main.func_extract`, and
  `main.func_replace`.
- All three tests pass under the MyLite MTR smoke profile.
- The only upstream test-source normalization is profile-specific default
  engine text normalization.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- The curated list is still small and baseline-only. A later MTR-scale
  comparison design must decide how to replay equivalent SQL through MyLite's
  embedded API and routed storage without daemon-only assumptions.

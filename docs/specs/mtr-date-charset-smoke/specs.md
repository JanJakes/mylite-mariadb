# MTR Date Format And ASCII Charset Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.date_formats`, `main.datetime_456`, and `main.ctype_ascii`. This adds
curated embedded baseline coverage for date parsing/formatting, out-of-range
datetime handling, locale-sensitive date names, and a compact ASCII character
set comparison matrix.

## Non-Goals

- Broad date/time, timezone, collation, or character-set MTR coverage.
- Normalizing tests that depend on disabled native engines, disabled SQL
  functions, disabled Sequence support, or repeated default-engine output
  differences.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/date_formats.test` exercises `STR_TO_DATE()`,
  `GET_FORMAT()`, `DATE_FORMAT()`, `TIME_FORMAT()`, locale-dependent date
  names, fractional-second parsing, invalid format input, and date/time result
  type inference through CTAS and `DESCRIBE`.
- `mariadb/mysql-test/main/datetime_456.test` covers an out-of-range datetime
  regression around adding one second past `9999-12-31 23:59:59` and
  `FROM_DAYS()` near the upper datetime range.
- `mariadb/mysql-test/main/ctype_ascii.test` exercises ASCII connection
  character-set setup, scalar comparisons, table-backed ASCII character
  equality, ordering, and escaped control-character values.
- All three tests pass under the MyLite MTR smoke profile without source
  changes.
- Nearby candidates are not yet suitable for the curated list:
  `main.bool` is skipped because the profile disables the Sequence engine,
  `main.null` and `main.ctype_latin1` require native MyISAM sections, and
  `main.ctype_binary` depends on the disabled `BENCHMARK()` function. Broader
  utf8mb3/utf8mb4 collation tests need repeated profile-specific
  `SHOW CREATE TABLE` normalization before they should enter the curated list.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected date-format and ASCII charset behavior in addition to existing
bootstrap, CAST/CONVERT, CASE-family, numeric/date, parser/comment/comparison,
scalar operator, string/format, and aggregate DISTINCT coverage. This remains
curated MariaDB embedded baseline coverage, not broad MTR-scale comparison and
not MyLite storage-routing evidence.

## Design

Add `main.date_formats`, `main.datetime_456`, and `main.ctype_ascii` to
`tools/mylite-mtr-harness`'s default curated list. No MariaDB test-source
normalization is needed for these cases.

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

- `tools/mylite-mtr-harness run main.date_formats`
- `tools/mylite-mtr-harness run main.datetime_456`
- `tools/mylite-mtr-harness run main.ctype_ascii`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes `main.date_formats`,
  `main.datetime_456`, and `main.ctype_ascii`.
- All three tests pass under the MyLite MTR smoke profile.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- Broader charset and collation MTR expansion likely needs a deliberate
  normalization policy for tests whose only current drift is the profile's
  Aria default engine and `PAGE_CHECKSUM=1` output.

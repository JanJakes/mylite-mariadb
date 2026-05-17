# MTR Type And Temporal Rounding Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.type_ranges`, `main.type_num`, `main.type_uint`, `main.type_year`,
`main.func_time_round`, `main.type_date_round`, `main.type_datetime_round`,
`main.type_time_round`, and `main.type_timestamp_round`. This adds curated
embedded baseline coverage for numeric and temporal type handling without
claiming broad MTR-scale compatibility.

## Non-Goals

- Broad numeric, decimal, temporal, charset, or function MTR coverage.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.
- Normalizing tests that have broad repeated default-engine output drift or
  depend on disabled native engines/server surfaces.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/type_ranges.test` covers range behavior across
  integer, floating-point, temporal, BLOB, ENUM, SET, key, CTAS, ALTER, and
  comparison paths.
- `mariadb/mysql-test/main/type_num.test` exercises numeric type conversion,
  comparison, and error/warning behavior for representative integer and
  floating-point forms.
- `mariadb/mysql-test/main/type_uint.test` exercises unsigned integer inserts,
  unsigned BIGINT/MEDIUMINT coercion through `COALESCE()` and `UNION`, DATE vs
  unsigned comparison behavior, unusable-key notes, and `NULLIF()` conversion.
- `mariadb/mysql-test/main/type_year.test` exercises `YEAR` storage,
  comparison, conversion, deprecated display widths, CTAS inference, rounding
  function type metadata, and `COALESCE()` encoding.
- `mariadb/mysql-test/main/func_time_round.test` and the four
  `type_*_round` tests exercise fractional temporal rounding over function and
  type-conversion paths.
- `main.type_ranges`, `main.type_num`, `main.func_time_round`,
  `main.type_date_round`, `main.type_datetime_round`, `main.type_time_round`,
  and `main.type_timestamp_round` pass under the MyLite MTR smoke profile
  without source changes.
- `main.type_uint` and `main.type_year` otherwise pass with the same narrow
  `ENGINE=Aria` / `ENGINE=MyISAM` and `PAGE_CHECKSUM=1`
  `SHOW CREATE TABLE` normalization already used by existing curated MTR tests.
- Nearby candidates remain outside the curated list:
  - `main.func_if` and `main.func_date_add` explicitly request disabled native
    MyISAM.
  - `main.func_time`, `main.type_date`, `main.type_datetime`, and
    `main.type_timestamp` reach explicit MyISAM or host-file/server-surface
    sections.
  - `main.type_int` has broad repeated `SHOW CREATE TABLE` default-engine
    output drift and should wait for a less noisy normalization policy.
  - `main.type_float` and `main.type_newdecimal` include explicit disabled
    MyISAM sections.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected numeric, type, date, and temporal-rounding behavior in addition to
existing bootstrap, CAST/CONVERT, CASE-family, parser/comment/comparison,
operator, scalar-function, default-expression, weight-string, string/format,
aggregate DISTINCT, date-format, ASCII charset, KDF, and disabled-DES
coverage. This remains curated MariaDB embedded baseline coverage, not broad
MTR-scale comparison and not MyLite storage-routing evidence.

## Design

- Add the selected type and temporal-rounding tests to
  `tools/mylite-mtr-harness`'s default curated list.
- Add profile-sensitive `--replace_result` directives immediately before the
  affected `SHOW CREATE TABLE` outputs in `main.type_uint` and
  `main.type_year`.
- Keep tests that require explicit native MyISAM or host-file/server surfaces
  outside the list.

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
- `tools/mylite-mtr-harness run main.type_ranges main.type_num main.type_uint main.type_year`
- `tools/mylite-mtr-harness run main.func_time_round main.type_date_round main.type_datetime_round main.type_time_round main.type_timestamp_round`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes the selected type and temporal-rounding
  tests.
- All selected tests pass under the MyLite MTR smoke profile.
- The only upstream test-source normalization is profile-specific default
  engine text normalization in `main.type_uint` and `main.type_year`.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- The curated list is still baseline-only. A later MTR-scale comparison design
  must decide how to replay equivalent SQL through MyLite's embedded API and
  routed storage without daemon-only assumptions.
- Repeated default-engine output drift in larger type tests would be better
  handled by a deliberate normalization policy than by adding many ad hoc
  directives.

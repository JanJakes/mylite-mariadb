# MTR Temporal Function Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.func_sapdb`, `main.func_time_64`, `main.func_timestamp`,
`main.in_datetime_241`, `main.str_to_datetime_457`, and
`main.type_time_6065`. This adds curated embedded baseline coverage for
temporal function, conversion, boundary, and indexed comparison behavior
without claiming broad MTR-scale compatibility.

## Non-Goals

- Broad temporal, timezone, optimizer, partition, sequence, or function MTR
  coverage.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.
- Normalizing tests that depend on disabled native engines, partitioning,
  sequence tables, daemon-only options, or host timezone state.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/func_sapdb.test` covers microsecond temporal
  extraction, `DATE_ADD()` / `DATE_SUB()`, `ADDTIME()` / `SUBTIME()`,
  `TIMEDIFF()`, `MAKETIME()`, `TIMESTAMP()`, `DATE()`, `TIME()`,
  `STR_TO_DATE()`, and table-backed temporal function result metadata.
- `mariadb/mysql-test/main/func_time_64.test` sources
  `include/have_64bit_timestamp.inc` and covers 64-bit timestamp boundary
  behavior for `FROM_UNIXTIME()`, `UNIX_TIMESTAMP()`, `CONVERT_TZ()`,
  timestamp range inserts, fractional rounding, and `ALTER TABLE` timestamp
  precision changes.
- `mariadb/mysql-test/main/func_timestamp.test` covers timezone-sensitive
  `UNIX_TIMESTAMP()` conversion over indexed table rows.
- `mariadb/mysql-test/main/in_datetime_241.test` covers an MDEV-241
  subquery/`IN` regression over `DATE` values.
- `mariadb/mysql-test/main/str_to_datetime_457.test` covers MDEV-457 temporal
  string-to-date/time/datetime parsing, fractional seconds, zero-date
  conversion, and backward-compatible loose temporal string forms.
- `mariadb/mysql-test/main/type_time_6065.test` covers MDEV-6065 and
  MDEV-15262 time/datetime comparisons across indexed and non-indexed access
  paths with forced and ignored index hints.
- The selected tests pass under the MyLite MTR smoke profile without source
  changes.
- Nearby candidates remain outside the curated list:
  - `main.func_time` and `main.func_date_add` reach explicit disabled native
    MyISAM sections.
  - `main.func_time_32` depends on a 32-bit timestamp feature check and would
    not provide coverage on the current 64-bit build.
  - `main.type_datetime_hires` and `main.sargable_date_cond` require
    partition or sequence support disabled in the current MTR smoke profile.
  - `main.sysdate_is_now` fails in embedded MTR because `SLEEP()` resolves as
    a missing stored function under the current profile.
  - Broader timezone tests depend on host timezone table/state assumptions and
    should be admitted separately with explicit environment evidence.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected temporal function, conversion, boundary, and indexed comparison
behavior in addition to existing bootstrap, CAST/CONVERT, CASE-family,
numeric/type/date/temporal-rounding, parser/comment/comparison, operator,
scalar-function, default-expression, weight-string, string/format, aggregate
DISTINCT, date-format, ASCII charset, KDF, disabled-DES, and REGEXP coverage.
This remains curated MariaDB embedded baseline coverage, not broad MTR-scale
comparison and not MyLite storage-routing evidence.

## Design

- Add the selected temporal tests to `tools/mylite-mtr-harness`'s default
  curated list.
- Keep explicit disabled-engine, skipped-feature, partition, sequence, and
  host-environment-sensitive candidates outside the list.
- Avoid MariaDB test-source edits for this slice because all admitted cases
  pass under the profile as-is.

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
- `tools/mylite-mtr-harness run main.func_sapdb main.func_time_64 main.func_timestamp main.in_datetime_241 main.str_to_datetime_457 main.type_time_6065`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes `main.func_sapdb`, `main.func_time_64`,
  `main.func_timestamp`, `main.in_datetime_241`, `main.str_to_datetime_457`,
  and `main.type_time_6065`.
- All selected tests pass under the MyLite MTR smoke profile.
- No upstream MariaDB test-source normalization is needed for this slice.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- This does not cover the broader timezone suite, high-resolution temporal type
  suites, or sargable date-condition partition paths; those need separate
  admission decisions because they depend on host environment or disabled
  profile features.
- The selected indexed comparison test still runs against MariaDB's embedded
  baseline, not MyLite's storage-routing index implementation.

# MTR Type Edge Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.type_char`, `main.type_varbinary`, and `main.type_interval`. This adds
curated embedded baseline coverage for character-to-numeric cast warnings,
VARBINARY comparison/index behavior, and interval casts.

## Non-Goals

- Broad data-type MTR coverage.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.
- Normalizing broad type suites that depend on disabled native engines,
  server-side file I/O, GIS functions, trimmed server functions, big-test
  variants, or large `SHOW CREATE TABLE` matrices.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/type_char.test` covers `CHAR` values cast to
  `DOUBLE`, `DECIMAL`, signed integer, and unsigned integer forms, including
  warning-sensitive string content.
- `mariadb/mysql-test/main/type_varbinary.test` covers `BETWEEN` predicates
  over a unique `VARBINARY(10)` column, both through the unique key and with
  `IGNORE KEY`.
- `mariadb/mysql-test/main/type_interval.test` covers `CAST(... AS INTERVAL
  DAY_SECOND(6))` from representative `VARCHAR` and `DECIMAL` values.
- All selected tests pass under the MyLite MTR smoke profile without upstream
  source changes.
- Probed nearby type suites stay outside this slice:
  - `main.type_float`, `main.type_bool`, `main.type_enum`, `main.type_set`,
    `main.type_temporal_mariadb53`, `main.type_time`, and
    `main.type_datetime` require disabled native MyISAM sections.
  - `main.type_blob`, `main.type_datetime_hires`, `main.type_time_hires`, and
    `main.type_timestamp_hires` require disabled InnoDB startup options.
  - `main.type_date` and `main.type_timestamp` reach unsupported
    `SELECT ... INTO OUTFILE`.
  - `main.type_decimal` depends on old MyISAM fixture files that are not
    available in this smoke profile.
  - `main.type_newdecimal` reaches trimmed GIS functions.
  - `main.type_newdecimal-big` is big-test-only and is treated as no coverage
    by the harness.
  - `main.func_hybrid_type` reaches the unsupported embedded `SLEEP()`
    function path.
  - `main.type_int`, `main.type_binary`, `main.type_varchar`,
    `main.type_hex_hybrid`, `main.type_nchar`, and
    `main.type_varchar_mysql41` need separate output-normalization review.
  - `main.type_json` needs separate disabled-engine and normalization review.
  - `main.type_row` currently diverges on the profile's maximum key length for
    a `TEXT UNIQUE` case.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected numeric, character/binary, interval, and type behavior in addition to
existing bootstrap, CAST/CONVERT, CASE-family, date/temporal,
parser/comment/comparison, `IN` predicate, operator, scalar-function,
default-expression, weight-string, string/format, aggregate DISTINCT,
date-format, charset, collation, KDF, disabled-DES, and REGEXP coverage. This
remains curated MariaDB embedded baseline coverage, not broad MTR-scale
comparison and not MyLite storage-routing evidence.

## Design

- Add the selected type tests to `tools/mylite-mtr-harness`'s default curated
  list.
- Do not modify upstream test files for this slice.
- Keep broader type suites with disabled native engine, trimmed server surface,
  big-test, or large normalization requirements outside the list.

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
- `tools/mylite-mtr-harness run main.type_char main.type_interval main.type_varbinary`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes the selected type edge tests.
- All selected tests pass under the MyLite MTR smoke profile.
- No upstream MariaDB test files are modified for this slice.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- Broader type suites need separate disabled-engine, unsupported-surface,
  fixture, and normalization policies before admission.
- This remains MariaDB embedded baseline coverage and does not prove MyLite
  storage-routing type behavior.

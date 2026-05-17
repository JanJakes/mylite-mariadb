# MTR Collation And Diagnostics Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.ctype_collate_database`, `main.ctype_collate_implicit`,
`main.ctype_collate_implicit_def`, `main.ctype_collate_table`, and
`main.ctype_errors`. This adds curated embedded baseline coverage for
session-configured default collations, database/table collation clauses,
implicit expression collations, charset-sensitive diagnostics, and localized
error-message behavior.

## Non-Goals

- Broad charset, collation, UCA, Unicode, or locale MTR coverage.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.
- Enabling disabled native engines or admitting broad charset suites with
  explicit InnoDB/MyISAM sections.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/ctype_collate_database.test` covers
  `character_set_collations` effects on database-level character set and
  collation declarations, including `SHOW CREATE DATABASE` output.
- `mariadb/mysql-test/main/ctype_collate_table.test` covers table-level
  `CHARACTER SET`, `COLLATE`, `DEFAULT`, and conflict handling across latin1
  and utf8mb4 declarations.
- `mariadb/mysql-test/main/ctype_collate_implicit.test` covers
  `character_set_collations` parsing, `SET NAMES`, implicit literal and
  expression collations, view definitions, information-schema exposure,
  prepared statement behavior, and `SHOW CREATE TABLE` output.
- `mariadb/mysql-test/main/ctype_collate_implicit_def.test` covers default
  collation selection for implicit string operations under the session
  `character_set_collations` variable.
- `mariadb/mysql-test/main/ctype_errors.test` covers charset-sensitive and
  localized diagnostics through `lc_messages`, `character_set_results`, and
  `SHOW WARNINGS`.
- `main.ctype_collate_database`, `main.ctype_collate_implicit_def`, and
  `main.ctype_errors` pass under the MyLite MTR smoke profile without source
  changes.
- `main.ctype_collate_implicit` and `main.ctype_collate_table` otherwise pass
  with the same narrow `ENGINE=Aria` / `ENGINE=MyISAM` and `PAGE_CHECKSUM=1`
  `SHOW CREATE TABLE` normalization already used by existing curated MTR
  tests.
- Nearby charset candidates remain outside the curated list:
  - `main.ctype_utf8` and `main.ctype_utf8mb4` explicitly require native
    InnoDB/MyISAM sections and contain broad `SHOW CREATE TABLE` matrices.
  - `main.ctype_latin1`, `main.ctype_binary`, and `main.ctype_collate` include
    broader common include files and should be admitted independently.
  - Partition, binlog, and engine-specific charset tests stay outside the
    current embedded profile.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected charset diagnostics and collation-default behavior in addition to
existing bootstrap, CAST/CONVERT, CASE-family, numeric/type/date/temporal,
parser/comment/comparison, `IN` predicate, operator, scalar-function,
default-expression, weight-string, string/format, aggregate DISTINCT,
date-format, ASCII charset, KDF, disabled-DES, and REGEXP coverage. This
remains curated MariaDB embedded baseline coverage, not broad MTR-scale
comparison and not MyLite storage-routing evidence.

## Design

- Add the selected charset/collation tests to `tools/mylite-mtr-harness`'s
  default curated list.
- Add profile-sensitive `--replace_result` directives immediately before the
  affected `SHOW CREATE TABLE` outputs in `main.ctype_collate_implicit` and
  `main.ctype_collate_table`.
- Keep broader charset suites with disabled native engine or large
  normalization requirements outside the list.

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
- `tools/mylite-mtr-harness run main.ctype_collate_database main.ctype_collate_implicit main.ctype_collate_implicit_def main.ctype_collate_table main.ctype_errors`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes the selected charset/collation tests.
- All selected tests pass under the MyLite MTR smoke profile.
- Upstream test-source normalization is limited to profile-specific default
  engine text around `SHOW CREATE TABLE`.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- Broader charset suites need a separate normalization and disabled-engine
  policy before admission.
- This remains MariaDB embedded baseline coverage and does not prove MyLite
  storage-routing collation behavior.

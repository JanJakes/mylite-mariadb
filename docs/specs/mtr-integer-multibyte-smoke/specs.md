# MTR Integer And Multibyte Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.type_int`, `main.ctype_mb`, and `main.ctype_recoding`. This adds curated
embedded baseline coverage for integer type inference and rounding metadata,
multibyte charset ALTER behavior, and charset recoding across identifiers,
defaults, expressions, and comparisons.

## Non-Goals

- Broad integer, charset, Unicode, or collation MTR coverage.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.
- Normalizing tests that rely on legacy `.frm` fixture import, table repair,
  disabled native MyISAM, host-file SQL I/O, or unrelated profile variables.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/type_int.test` covers integer metadata and
  expression behavior, including signed/unsigned integer-to-string inference,
  integer rounding result types, and large unsigned rounding edge cases.
- `mariadb/mysql-test/main/ctype_mb.test` covers multibyte `CREATE TABLE ...
  SELECT`, `ALTER TABLE` character-set changes, prefix-key metadata, and
  warning-sensitive multibyte truncation.
- `mariadb/mysql-test/main/ctype_recoding.test` covers charset recoding for
  column values, table and column identifiers, defaults, binary and UTF-8
  connection modes, and automatic charset conversion in expressions.
- Under the MyLite MTR smoke profile, the selected tests differ from upstream
  expectations only where `SHOW CREATE TABLE` reports the profile's default
  Aria engine and `PAGE_CHECKSUM=1` instead of upstream MyISAM text.
- `type_int.test` has `SHOW CREATE TABLE` inside stored procedures, so the
  normalization must be attached to the surrounding `CALL p1(...)` statements
  rather than inserted into the procedure body as mysqltest commands.
- Probed nearby candidates remain outside this slice:
  - `main.type_varchar_mysql41` relies on legacy `.frm` fixture import and
    repair/upgrade paths.
  - `main.ctype_utf16_def` diverges on the profile-specific
    `ft_stopword_file` variable, not just default-engine text.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected integer metadata/rounding behavior and multibyte charset recoding in
addition to existing numeric, type, charset, collation, weight-string, and LIKE
condition-propagation coverage. This remains curated MariaDB embedded baseline
coverage, not broad MTR-scale comparison and not MyLite storage-routing
evidence.

## Design

- Add the selected tests to `tools/mylite-mtr-harness`'s default curated list.
- Add profile-sensitive `--replace_result` directives immediately before
  affected top-level `SHOW CREATE TABLE` statements.
- For `type_int.test` stored-procedure output, add the same replacement before
  each `CALL p1(...)` whose result includes a nested `SHOW CREATE TABLE`.
- Keep normalization limited to mapping `ENGINE=Aria` to `ENGINE=MyISAM` and
  removing ` PAGE_CHECKSUM=1`.

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
- `tools/mylite-mtr-harness run main.ctype_mb main.ctype_recoding main.type_int`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes the selected tests.
- All selected tests pass under the MyLite MTR smoke profile.
- Upstream test-source normalization is limited to profile-specific default
  engine text around affected `SHOW CREATE TABLE` outputs.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- The added normalization touches upstream-derived MTR tests, so later MTR
  expansion should continue verifying that it only hides the configured
  default-engine drift.
- This remains MariaDB embedded baseline coverage and does not prove MyLite
  storage-routing integer or charset behavior.

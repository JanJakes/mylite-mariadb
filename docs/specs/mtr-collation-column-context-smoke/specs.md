# MTR Collation Column And Context Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.ctype_collate_column` and `main.ctype_collate_context`. This adds
curated embedded baseline coverage for column-level collation declarations,
`COLLATE DEFAULT`, expression/context collation resolution, and prepared
statement parameter collation behavior.

## Non-Goals

- Broad charset, UCA, Unicode, or locale MTR coverage.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.
- Enabling disabled native engines or admitting broad charset suites with
  explicit native-engine sections.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/ctype_collate_column.test` covers column-level
  `COLLATE DEFAULT`, conflicting column collation declarations, binary column
  collation declarations, and a generated matrix of character set and
  collation clauses across column, table, and database contexts.
- `mariadb/mysql-test/main/ctype_collate_context.test` covers collation
  derivation from explicit contexts, `COLLATE` with `NULL` and bound
  parameters, `CONVERT(NULL USING ...)`, and prepared `CREATE TABLE ... AS
  SELECT` statements whose result column collation depends on the parameter
  context.
- Both selected tests otherwise pass with the same narrow `ENGINE=Aria` /
  `ENGINE=MyISAM` and `PAGE_CHECKSUM=1` `SHOW CREATE TABLE` normalization
  already used by existing curated MTR tests.
- Nearby broader charset candidates remain outside the curated list until they
  have separate normalization and disabled-engine policy.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected charset diagnostics, collation-default behavior, and column/context
collation behavior in addition to existing bootstrap, CAST/CONVERT,
CASE-family, numeric/type/date/temporal, parser/comment/comparison, `IN`
predicate, operator, scalar-function, default-expression, weight-string,
string/format, aggregate DISTINCT, date-format, ASCII charset, KDF,
disabled-DES, and REGEXP coverage. This remains curated MariaDB embedded
baseline coverage, not broad MTR-scale comparison and not MyLite
storage-routing evidence.

## Design

- Add the selected collation tests to `tools/mylite-mtr-harness`'s default
  curated list.
- Add profile-sensitive `--replace_result` directives immediately before the
  affected `SHOW CREATE TABLE` outputs in `main.ctype_collate_column` and
  `main.ctype_collate_context`.
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
- `tools/mylite-mtr-harness run main.ctype_collate_column main.ctype_collate_context`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes the selected column/context collation
  tests.
- All selected tests pass under the MyLite MTR smoke profile.
- Upstream test-source normalization is limited to profile-specific default
  engine text around `SHOW CREATE TABLE`.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- Broader charset suites need a separate normalization and disabled-engine
  policy before admission.
- This remains MariaDB embedded baseline coverage and does not prove MyLite
  storage-routing collation behavior.

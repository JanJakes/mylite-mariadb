# MTR CASE Expression Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with one more exact upstream test:
`main.case`. This adds curated baseline coverage for CASE, COALESCE, IFNULL,
collation aggregation, selected grouping, joins, temporal values, and result
metadata without claiming broad MTR-scale compatibility.

## Non-Goals

- Running MTR against MyLite storage-engine routing.
- Adding MTR to the default compatibility harness group set.
- Enabling skipped suites that require disabled engines or server surfaces.
- Normalizing broad result differences outside this selected test.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/case.test` exercises scalar and searched CASE
  expressions, COALESCE, IFNULL, mixed type aggregation, collation errors,
  grouping, joins, temporal values, and selected result metadata.
- `mariadb/mysql-test/main/case.result` expects the upstream default MyISAM
  engine in `SHOW CREATE TABLE` output.
- The MyLite MTR smoke profile intentionally disables native MyISAM and runs
  with Aria as the default MTR storage engine. The profile-specific difference
  for this test is `ENGINE=Aria ... PAGE_CHECKSUM=1` in `SHOW CREATE TABLE`
  output, so the test can use the same narrow normalization already used by
  `main.cast`.
- MTR's `--do-test` value is a pattern. Unanchored names can select sibling
  tests such as `main.bool_innodb`, so the MyLite harness should anchor exact
  curated case names.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
scalar CAST/CONVERT and CASE-family expression behavior. This remains baseline
MariaDB MTR coverage, not proof that MyLite storage routing passes upstream
MTR suites.

## Design

- Add `main.case` to `tools/mylite-mtr-harness`'s default curated list.
- Anchor selected test names in the harness with `^...$` so a curated
  `suite.test` argument does not accidentally run similarly named tests.
- Add two `--replace_result` lines to `mariadb/mysql-test/main/case.test`
  before the profile-sensitive `SHOW CREATE TABLE` statements.

## File Lifecycle

No MyLite `.mylite` file format or runtime lifecycle changes. The MTR runner
continues to use `build/mariadb-mtr-smoke/mysql-test/var` for temporary test
state.

## Embedded Lifecycle And API

No `libmylite` API change. The slice only expands opt-in MariaDB embedded MTR
baseline coverage.

## Build, Size, And Dependencies

No dependency or production binary-size change. The opt-in MTR build tree can
still be reclaimed with `rm -rf build/mariadb-mtr-smoke` or `rm -rf build`.

## Test Plan

- `tools/mylite-mtr-harness list`
- `tools/mylite-mtr-harness run main.case`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes `main.case`.
- Selected MTR case names are exact enough not to pick sibling tests.
- `main.case` passes under the MyLite MTR smoke profile.
- Documentation keeps the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- Future curated MTR cases may require additional profile-specific
  normalization or may depend on disabled server surfaces. Each case should be
  admitted only after a targeted run.

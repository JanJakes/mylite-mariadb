# MTR DEFAULT And Weight String Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.func_default` and `main.func_weight_string`. This adds curated embedded
baseline coverage for `DEFAULT()` expression behavior and `WEIGHT_STRING()`
type/weight generation without claiming broad MTR-scale compatibility.

## Non-Goals

- Broad default-expression, collation, or weight-string MTR coverage.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.
- Normalizing optimizer row estimates or disabled server-surface behavior.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/func_default.test` exercises `DEFAULT()` over
  literal defaults, timestamp defaults, views, derived tables, text/blob
  defaults, updates, and information-schema column default visibility.
- `mariadb/mysql-test/main/func_weight_string.test` exercises
  `WEIGHT_STRING()` over character and binary forms, level modifiers, type
  inference for `CREATE TABLE ... SELECT`, null inputs, numeric conversion,
  generated defaults, and hex output.
- The MyLite MTR smoke profile intentionally disables native MyISAM and runs
  with Aria as the default MTR storage engine. Both selected tests otherwise
  pass with the same narrow `ENGINE=Aria` / `ENGINE=MyISAM` and
  `PAGE_CHECKSUM=1` `SHOW CREATE TABLE` normalization already used by
  existing curated MTR tests.
- Nearby candidates remain outside the curated list:
  - `main.func_in` has optimizer row-estimate drift under the profile.
  - `main.func_group` explicitly requests disabled native MyISAM.
  - `main.func_hybrid_type` reaches trimmed `SLEEP()` behavior.
  - `main.func_misc` is skipped because the metadata-lock-info plugin is not
    built in the profile.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected default-expression and weight-string behavior in addition to existing
bootstrap, CAST/CONVERT, CASE-family, numeric/date,
parser/comment/comparison, scalar-operator, scalar-function, string/format,
aggregate DISTINCT, date-format, ASCII charset, KDF, and disabled-DES
coverage. This remains curated MariaDB embedded baseline coverage, not broad
MTR-scale comparison and not MyLite storage-routing evidence.

## Design

- Add `main.func_default` and `main.func_weight_string` to
  `tools/mylite-mtr-harness`'s default curated list.
- Add profile-sensitive `--replace_result` directives immediately before the
  affected `SHOW CREATE TABLE` outputs in the selected upstream tests.
- Keep failed candidates out of the list until their disabled-surface or
  optimizer-stability issues are addressed deliberately.

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
- `tools/mylite-mtr-harness run main.func_default main.func_weight_string`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes `main.func_default` and
  `main.func_weight_string`.
- Both tests pass under the MyLite MTR smoke profile.
- The only upstream test-source normalization is profile-specific default
  engine text normalization.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- The curated list is still small and baseline-only. A later MTR-scale
  comparison design must decide how to replay equivalent SQL through MyLite's
  embedded API and routed storage without daemon-only assumptions.

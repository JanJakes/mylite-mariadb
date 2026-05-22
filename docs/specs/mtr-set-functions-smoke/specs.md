# MTR SET-Function Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.func_set`. This adds
upstream embedded baseline coverage for `INTERVAL()`, `FIELD()`, `ELT()`,
`FIND_IN_SET()`, `MAKE_SET()`, `EXPORT_SET()`, SET-column conversion, NULL and
row-operand diagnostics, and selected date/interval coercion behavior.

## Non-Goals

- Broad scalar-function MTR promotion.
- Running MTR against MyLite storage-engine routing.
- Changing MyLite SQL behavior, metadata routing, storage behavior, or file
  format.
- Promoting native MyISAM behavior or disabled server-oriented surfaces.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/func_set.test` covers SET-family scalar functions,
  `INTERVAL()` numeric/date coercions, binary `FIND_IN_SET()`, SET-column
  conversion, row-operand errors, and MDEV/bug regressions around assertions and
  warnings.
- The only observed probe blocker is the explicit lowercase `engine=myisam`
  clause in the Bug#44367 section. That section uses a simple SET-column table
  to exercise `find_in_set(f1,f1)` over ordinary rows; it does not assert
  MyISAM-specific behavior, indexes, row format, repair behavior, or metadata.
- Existing admitted MTR tests normalize Aria/MyISAM text where the smoke
  profile intentionally runs with Aria instead of native MyISAM.
- Probe evidence before the slice:
  `tools/mylite-mtr-harness probe main.func_set` fails at the one
  `engine=myisam` table with `ER_UNKNOWN_STORAGE_ENGINE`.

## Design

- Add lowercase Aria/MyISAM result normalization for the one engine-sensitive
  statement.
- Change that scratch table to `engine=Aria`, the MTR smoke profile's available
  persistent engine.
- Add `main.func_set` to the curated MTR smoke list beside related scalar
  function tests.
- Keep docs scoped to selected SET-family scalar-function behavior.

## Compatibility Impact

The curated MTR smoke runner gains upstream coverage for selected SET-family
scalar-function behavior. This is MariaDB embedded baseline evidence, not a new
claim about MyLite storage-engine routing.

## Single-File And Lifecycle Impact

No MyLite `.mylite` file lifecycle change. The test runs in the MTR smoke
vardir.

## Public API And File-Format Impact

No public API or durable file-format change.

## Storage-Engine Routing Impact

No routing change. The test runs with the MTR smoke profile's embedded runtime,
where native MyISAM is deliberately unavailable and ordinary MTR scratch tables
use Aria for this compatibility smoke.

## Build, Size, And Dependencies

No production dependency, binary-size, or default-build change.

## Test Plan

- `tools/mylite-mtr-harness probe main.func_set`
- `tools/mylite-mtr-harness run main.func_set`
- `tools/mylite-mtr-harness run`
- `tools/mylite-mtr-harness list | wc -l`
- `bash -n tools/mylite-mtr-harness`
- `find . -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- `main.func_set` reports an MTR pass under strict harness execution.
- The full curated MTR smoke suite remains green.
- No `.reject` files remain.
- Docs scope the claim to selected SET-family scalar-function behavior.

## Verification Results

- `tools/mylite-mtr-harness probe main.func_set`: passed.
- `tools/mylite-mtr-harness run main.func_set`: passed.
- `tools/mylite-mtr-harness run`: passed bootstrap plus all 159 selected
  `main` tests.
- `tools/mylite-mtr-harness list | wc -l`: `160`.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find . -name '*.reject' -print`: no output.
- `git diff --check`: passed.

## Risks And Follow-Up

The test includes current-date-dependent `FIND_IN_SET(DAYOFWEEK(CURRENT_DATE()))`
cases, but those are upstream MTR baseline expectations and do not touch MyLite
storage routing. Broader SET, enum, and application-schema coverage should
continue through probe-backed slices.

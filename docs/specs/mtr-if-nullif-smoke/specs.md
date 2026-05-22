# MTR IF/NULLIF Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.func_if`. This adds
upstream embedded baseline coverage for selected `IF()`, `NULLIF()`, conditional
type resolution, ordering, aggregate, subquery, warning, BLOB, date-format, and
charset-sensitive expression behavior.

## Non-Goals

- Broad scalar-function MTR promotion.
- Running MTR against MyLite storage-engine routing.
- Changing MyLite SQL behavior, metadata routing, storage behavior, or file
  format.
- Promoting tests that depend on native MyISAM behavior, disabled server
  surfaces, host-file I/O, Sequence, or debug-only infrastructure.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/func_if.test` covers `IF()` truthiness and type
  selection, case-sensitive and binary string ordering, `NULLIF()` evaluation,
  warning production, aggregate interaction, `FROM_UNIXTIME()` ordering, nested
  `IF()` parse cost, long-text numeric casts, grouped `LONGBLOB` conditionals,
  subquery operands, and MDEV-8663 row-expression output shape.
- The observed MyLite MTR smoke failure is the single explicit
  `ENGINE=MyISAM` clause used in the case-sensitivity section. The expected
  behavior in that section is expression and ordering behavior over ordinary
  persistent rows; the test does not exercise MyISAM-specific table options,
  repair behavior, row format, indexes, or metadata.
- Existing admitted MTR tests normalize Aria/MyISAM text with
  `--replace_result ENGINE=Aria ENGINE=MyISAM " PAGE_CHECKSUM=1" ""` where the
  smoke profile intentionally runs with Aria instead of native MyISAM.
- Probe evidence before the slice:
  `tools/mylite-mtr-harness probe main.func_if` fails at
  `CREATE TABLE ... ENGINE=MyISAM` with `ER_UNKNOWN_STORAGE_ENGINE`.

## Design

- Keep the test result text stable by adding the existing Aria/MyISAM
  `--replace_result` normalization around the one engine-sensitive statement.
- Change only that `CREATE TABLE` to `ENGINE=Aria`, the MTR smoke profile's
  available persistent engine, because the explicit MyISAM dependency is not
  semantic to the `IF()` / `NULLIF()` coverage.
- Add `main.func_if` to the curated MTR smoke list beside the related scalar
  function tests.
- Keep documentation scoped to selected upstream IF/NULLIF expression behavior.

## Compatibility Impact

The curated MTR smoke runner gains upstream coverage for selected conditional
expression behavior. This is MariaDB embedded baseline evidence, not a new
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

- `tools/mylite-mtr-harness probe main.func_if`
- `tools/mylite-mtr-harness run main.func_if`
- `tools/mylite-mtr-harness run`
- `tools/mylite-mtr-harness list | wc -l`
- `bash -n tools/mylite-mtr-harness`
- `find . -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- `main.func_if` reports an MTR pass under strict harness execution.
- The full curated MTR smoke suite remains green.
- No `.reject` files remain.
- Docs scope the claim to selected conditional expression behavior.

## Verification Results

- `tools/mylite-mtr-harness probe main.func_if`: passed.
- `tools/mylite-mtr-harness run main.func_if`: passed.
- `tools/mylite-mtr-harness run`: passed bootstrap plus all 157 selected
  `main` tests.
- `tools/mylite-mtr-harness list | wc -l`: `158`.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find . -name '*.reject' -print`: no output.
- `git diff --check`: passed.

## Risks And Follow-Up

Using Aria for the one ordinary table is acceptable for this smoke because the
covered assertions are expression, warning, and ordering assertions. Future MTR
promotions should continue to reject tests where explicit native-engine clauses
exercise engine-specific behavior.

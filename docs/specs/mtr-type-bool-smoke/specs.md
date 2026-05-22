# MTR Type Bool Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.type_bool`. This adds
upstream embedded baseline coverage for the MDEV-35135 boolean-condition
regression involving aggregate output, `ROWNUM()`, `WHERE`, and `HAVING`
evaluation.

## Non-Goals

- Broad boolean, Oracle-mode, or aggregate MTR promotion.
- Running MTR against MyLite storage-engine routing.
- Changing MyLite SQL behavior, metadata routing, storage behavior, or file
  format.
- Promoting native MyISAM behavior or server-oriented surfaces.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/type_bool.test` is a focused MDEV-35135 regression
  for `Item_bool_func::val_int` / `do_select` assertion coverage.
- The only observed probe blocker is the explicit `ENGINE=MyISAM` scratch table.
  The table has one integer column, one inserted row, no MyISAM-specific
  options, no index, and no metadata assertions.
- Existing admitted MTR tests normalize Aria/MyISAM text with
  `--replace_result ENGINE=Aria ENGINE=MyISAM " PAGE_CHECKSUM=1" ""` where the
  smoke profile intentionally runs with Aria instead of native MyISAM.

## Design

- Add the standard Aria/MyISAM normalization before the scratch table creation.
- Change that table to `ENGINE=Aria`, the MTR smoke profile's available
  persistent engine.
- Add `main.type_bool` to the curated MTR smoke list near the other type tests.
- Keep docs scoped to selected boolean aggregate/HAVING expression behavior.

## Compatibility Impact

The curated MTR smoke runner gains upstream coverage for a selected boolean
expression regression. This is MariaDB embedded baseline evidence, not a new
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

- `tools/mylite-mtr-harness probe main.type_bool`
- `tools/mylite-mtr-harness run main.type_bool`
- `tools/mylite-mtr-harness run`
- `tools/mylite-mtr-harness list | wc -l`
- `bash -n tools/mylite-mtr-harness`
- `find . -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- `main.type_bool` reports an MTR pass under strict harness execution.
- The full curated MTR smoke suite remains green.
- No `.reject` files remain.
- Docs scope the claim to selected boolean aggregate/HAVING expression behavior.

## Verification Results

- `tools/mylite-mtr-harness probe main.type_bool`: passed.
- `tools/mylite-mtr-harness run main.type_bool`: passed.
- `tools/mylite-mtr-harness run`: passed bootstrap plus all 158 selected
  `main` tests.
- `tools/mylite-mtr-harness list | wc -l`: `159`.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find . -name '*.reject' -print`: no output.
- `git diff --check`: passed.

## Risks And Follow-Up

This slice intentionally admits only the tiny no-index scratch-table regression.
Broader boolean and Oracle-mode MTR surfaces should continue through explicit
probe review.

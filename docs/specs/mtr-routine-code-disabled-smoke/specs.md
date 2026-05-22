# MTR Routine Code Disabled Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.sp-no-code`. This adds
upstream embedded baseline coverage that `SHOW PROCEDURE CODE` and
`SHOW FUNCTION CODE` remain disabled in non-debug builds.

## Non-Goals

- Enabling stored routine debug-code inspection.
- Claiming default-product stored-program runtime or routine metadata support.
- Running MTR against MyLite storage-engine routing.
- Changing parser, stored-program, routine metadata, storage behavior, or file
  format.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/sp-no-code.test` sources `include/not_debug.inc`
  and expects `ER_FEATURE_DISABLED` for `SHOW PROCEDURE CODE foo` and
  `SHOW FUNCTION CODE foo`.
- `mariadb/sql/sql_yacc.yy` parses `SHOW PROCEDURE CODE` and
  `SHOW FUNCTION CODE` into `SQLCOM_SHOW_PROC_CODE` and
  `SQLCOM_SHOW_FUNC_CODE`.
- `mariadb/sql/sql_lex.cc` rejects routine-code inspection with
  `ER_FEATURE_DISABLED` unless the server is built with debug support.
- Probe evidence before admission:
  `tools/mylite-mtr-harness probe main.sp-no-code` passed without upstream
  test edits.

## Design

- Add `main.sp-no-code` near the prepared-statement and diagnostics MTR smoke
  tests in the curated harness list.
- Keep the upstream test and result files unchanged.
- Scope docs to disabled routine debug-code inspection, not stored routine
  runtime support.

## Compatibility Impact

The curated MTR smoke runner gains upstream embedded baseline coverage for a
disabled routine debug-code surface. This reinforces the server-surface policy
that debug-only routine introspection must not appear accidentally in the
default embedded profile.

## Single-File And Lifecycle Impact

No MyLite `.mylite` file lifecycle change. The test does not create durable
application tables.

## Public API And File-Format Impact

No public API or durable MyLite file-format change.

## Storage-Engine Routing Impact

No MyLite routing change.

## Build, Size, And Dependencies

No production dependency, binary-size, or default-build change.

## Test Plan

- `tools/mylite-mtr-harness probe main.sp-no-code`
- `tools/mylite-mtr-harness run main.sp-no-code`
- `tools/mylite-mtr-harness run`
- `tools/mylite-mtr-harness list | wc -l`
- `bash -n tools/mylite-mtr-harness`
- `find . -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- `main.sp-no-code` reports an MTR pass under strict harness execution.
- The full curated MTR smoke suite remains green.
- No `.reject` files remain.
- Docs scope the claim to disabled routine debug-code inspection.

## Verification Results

- `tools/mylite-mtr-harness probe main.sp-no-code`: passed.
- `tools/mylite-mtr-harness run main.sp-no-code`: passed.
- `tools/mylite-mtr-harness run`: passed bootstrap plus all 170 selected
  `main` tests.
- `tools/mylite-mtr-harness list | wc -l`: `171`.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find . -name '*.reject' -print`: no output.
- `git diff --check`: passed.

## Risks And Follow-Up

This test covers a debug-only rejection path, not routine runtime. Future
catalog-backed routine support still needs first-party product tests for DDL,
metadata, execution policy, and single-file lifecycle.

# MTR Missing Routine Diagnostic Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.sp_missing_4665`. This
adds upstream embedded baseline coverage that a missing stored-function
reference inside a subquery over an updatable view returns
`ER_SP_DOES_NOT_EXIST` instead of crashing.

## Non-Goals

- Claiming default-product view runtime support.
- Claiming default-product stored-program runtime or routine metadata support.
- Running MTR against MyLite storage-engine routing.
- Changing parser, view, stored-program, routine metadata, storage behavior,
  or file format.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/sp_missing_4665.test` creates a simple table and
  view, attempts `DELETE FROM v WHERE (SELECT g())`, and expects
  `ER_SP_DOES_NOT_EXIST` for the missing `test.g` function.
- The selected test exercises views as part of the MTR smoke profile. That is
  useful upstream baseline evidence, but does not change the default MyLite
  embedded profile's documented disabled view runtime.
- `mariadb/sql/item_func.cc` emits `ER_SP_DOES_NOT_EXIST` for unresolved
  stored-function references.
- `mariadb/sql/sql_parse.cc` contains stored procedure/function missing-routine
  diagnostics for callable routine paths.
- Probe evidence before admission:
  `tools/mylite-mtr-harness probe main.sp_missing_4665` passed without
  upstream test edits.

## Design

- Add `main.sp_missing_4665` near the routine-code and diagnostics MTR smoke
  tests in the curated harness list.
- Keep the upstream test and result files unchanged.
- Scope docs to selected missing-routine diagnostics under the MTR profile.

## Compatibility Impact

The curated MTR smoke runner gains upstream embedded baseline coverage for a
missing stored-function diagnostic. This is MariaDB embedded compatibility
evidence, not a new MyLite view-runtime or stored-program runtime claim.

## Single-File And Lifecycle Impact

No MyLite `.mylite` file lifecycle change. The test runs inside the MTR smoke
vardir and creates only transient MTR tables/views.

## Public API And File-Format Impact

No public API or durable MyLite file-format change.

## Storage-Engine Routing Impact

No MyLite routing change.

## Build, Size, And Dependencies

No production dependency, binary-size, or default-build change.

## Test Plan

- `tools/mylite-mtr-harness probe main.sp_missing_4665`
- `tools/mylite-mtr-harness run main.sp_missing_4665`
- `tools/mylite-mtr-harness run`
- `tools/mylite-mtr-harness list | wc -l`
- `bash -n tools/mylite-mtr-harness`
- `find . -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- `main.sp_missing_4665` reports an MTR pass under strict harness execution.
- The full curated MTR smoke suite remains green.
- No `.reject` files remain.
- Docs scope the claim to selected missing-routine diagnostics.

## Verification Results

- `tools/mylite-mtr-harness probe main.sp_missing_4665`: passed.
- `tools/mylite-mtr-harness run main.sp_missing_4665`: passed.
- `tools/mylite-mtr-harness run`: passed bootstrap plus all 171 selected
  `main` tests.
- `tools/mylite-mtr-harness list | wc -l`: `172`.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find . -name '*.reject' -print`: no output.
- `git diff --check`: passed.

## Risks And Follow-Up

This test uses MTR-profile view execution to reach the diagnostic path. Future
catalog-backed view and routine support still need first-party product tests
for DDL, metadata, execution policy, and single-file lifecycle.

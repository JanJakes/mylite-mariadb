# MTR Procedure DDL Runtime Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.create_drop_procedure`.
This adds upstream embedded baseline coverage for selected stored-procedure
DDL, procedure execution through `CALL`, routine metadata in `mysql.proc`,
`IF NOT EXISTS`, `OR REPLACE`, duplicate diagnostics, and drop behavior under
the MTR profile.

## Non-Goals

- Claiming default-product stored-program runtime or routine metadata support.
- Implementing catalog-backed MyLite routines.
- Adding stored-function coverage; `main.create_drop_function` is skipped for
  embedded server and remains unadmitted.
- Running MTR against MyLite storage-engine routing.
- Changing parser, stored-program, routine metadata, storage behavior, or file
  format.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/create_drop_procedure.test` creates a base table,
  creates and replaces a stored procedure, executes it with `CALL`, checks
  procedure comments in `mysql.proc`, and verifies duplicate/drop diagnostics.
- `mariadb/mysql-test/main/create_drop_function.test` is not a clean companion
  for this slice because MTR skips it for embedded server.
- `mariadb/sql/sql_yacc.yy` parses procedure DDL and `CALL` statements.
- `mariadb/sql/sql_parse.cc` dispatches procedure create/drop and call paths
  through stored-program handlers.
- `mariadb/sql/sp.cc` owns routine creation/drop through
  `Sp_handler::sp_create_routine()` and `Sp_handler::sp_drop_routine()`, plus
  routine lookup through `Sp_handler::sp_find_routine()`.
- `mariadb/sql/sp_head.cc` owns stored-procedure execution through
  `sp_head::execute_procedure()`.
- Probe evidence before admission:
  `tools/mylite-mtr-harness probe main.create_drop_procedure` passed without
  upstream test edits.

## Design

- Add `main.create_drop_procedure` near the view, trigger, and DDL/name MTR
  smoke tests in the curated harness list.
- Keep the upstream test and result files unchanged.
- Scope docs to MTR-profile stored-procedure DDL/runtime coverage. The default
  MyLite product profile still rejects persistent routines and `CALL` before
  MariaDB can publish filesystem/server-table metadata or execute routine
  bodies.

## Compatibility Impact

The curated MTR smoke runner gains upstream embedded baseline coverage for
selected stored-procedure DDL/runtime behavior under the MTR profile. This is
MariaDB embedded compatibility evidence, not a new MyLite stored-program
runtime, routine metadata, or catalog-backed routine support claim.

## Single-File And Lifecycle Impact

No MyLite `.mylite` file lifecycle change. The test runs inside the MTR smoke
vardir and creates only transient MTR table and routine metadata.

## Public API And File-Format Impact

No public API or durable MyLite file-format change.

## Storage-Engine Routing Impact

No MyLite routing change.

## Build, Size, And Dependencies

No production dependency, binary-size, or default-build change.

## Test Plan

- `tools/mylite-mtr-harness probe main.create_drop_procedure`
- `tools/mylite-mtr-harness run main.create_drop_procedure`
- `tools/mylite-mtr-harness run`
- `tools/mylite-mtr-harness list | wc -l`
- `bash -n tools/mylite-mtr-harness`
- `find . -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- `main.create_drop_procedure` reports an MTR pass under strict harness
  execution.
- The full curated MTR smoke suite remains green.
- No `.reject` files remain.
- Docs scope the claim to MTR-profile stored-procedure DDL/runtime behavior.

## Verification Results

- `tools/mylite-mtr-harness probe main.create_drop_procedure`: passed.
- `tools/mylite-mtr-harness run main.create_drop_procedure`: passed.
- `tools/mylite-mtr-harness run`: passed bootstrap plus all 176 selected
  `main` tests.
- `tools/mylite-mtr-harness list | wc -l`: `177`.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find . -name '*.reject' -print`: no output.
- `git diff --check`: passed.

## Risks And Follow-Up

This test intentionally exercises MariaDB's file-backed/server-table
stored-procedure runtime inside the opt-in MTR profile. Future catalog-backed
MyLite routine support still needs first-party product tests for DDL, metadata,
`CALL`, parameter binding, statement effects, transaction/rollback ordering,
and single-file lifecycle.

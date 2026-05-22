# MTR Compound Parser Diagnostic Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.sp-memory-leak`. This
adds upstream embedded baseline coverage for syntax diagnostics in compound
statements, including malformed `IF`, `WHILE`, `REPEAT`, and cursor `OPEN`
forms inside `BEGIN NOT ATOMIC` blocks.

## Non-Goals

- Claiming default-product stored-program runtime support.
- Running MTR against MyLite storage-engine routing.
- Changing parser, stored-program runtime, storage behavior, or file format.
- Proving memory-leak behavior outside the normal MTR pass/fail signal.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/sp-memory-leak.test` feeds malformed compound
  statement bodies through the parser and expects `ER_PARSE_ERROR` diagnostics
  around missing `THEN`, `DO`, `END`, and cursor `OPEN` argument syntax.
- `mariadb/sql/sql_yacc.yy` owns compound statement, cursor declaration, and
  cursor-open grammar.
- `mariadb/sql/sp_head.cc` and `mariadb/sql/sp_pcontext.cc` manage stored
  program parse state and compound-statement context while these syntax errors
  are reported.
- Probe evidence before admission:
  `tools/mylite-mtr-harness probe main.sp-memory-leak` passed without
  upstream test edits.

## Design

- Add `main.sp-memory-leak` near the routine/diagnostics MTR smoke tests in the
  curated harness list.
- Keep the upstream test and result files unchanged.
- Scope docs to selected compound-statement parser diagnostics, not
  stored-program runtime support.

## Compatibility Impact

The curated MTR smoke runner gains upstream embedded baseline coverage for
selected compound-statement parser diagnostics. This is MariaDB embedded
compatibility evidence, not a new MyLite stored-program runtime claim.

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

- `tools/mylite-mtr-harness probe main.sp-memory-leak`
- `tools/mylite-mtr-harness run main.sp-memory-leak`
- `tools/mylite-mtr-harness run`
- `tools/mylite-mtr-harness list | wc -l`
- `bash -n tools/mylite-mtr-harness`
- `find . -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- `main.sp-memory-leak` reports an MTR pass under strict harness execution.
- The full curated MTR smoke suite remains green.
- No `.reject` files remain.
- Docs scope the claim to selected compound-statement parser diagnostics.

## Verification Results

- `tools/mylite-mtr-harness probe main.sp-memory-leak`: passed.
- `tools/mylite-mtr-harness run main.sp-memory-leak`: passed.
- `tools/mylite-mtr-harness run`: passed bootstrap plus all 173 selected
  `main` tests.
- `tools/mylite-mtr-harness list | wc -l`: `174`.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find . -name '*.reject' -print`: no output.
- `git diff --check`: passed.

## Risks And Follow-Up

This test parses stored-program-shaped compound statements, but does not change
MyLite's default stored-program runtime policy. Future routine support still
needs first-party product tests for DDL, metadata, execution policy, and
single-file lifecycle.

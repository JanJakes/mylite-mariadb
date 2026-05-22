# MTR Mixed Set-Operation Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.set_operation`. This adds
upstream embedded baseline coverage for mixed `UNION`, `UNION ALL`, `EXCEPT`,
`EXCEPT ALL`, `INTERSECT`, and `INTERSECT ALL` execution, precedence,
bracketing, table value constructors, derived tables, views, prepared
statements, optimizer output, large intermediate inputs, and empty intermediate
results.

## Non-Goals

- Running MTR against MyLite storage-engine routing.
- Claiming MyLite storage-level set-operation behavior.
- Enabling native MyISAM or changing the MTR smoke profile's storage-engine
  policy.
- Claiming default-product view or prepared-statement behavior beyond the
  already documented direct/prepared SQL API surface.
- Changing SQL set-operation semantics, optimizer behavior, storage behavior,
  metadata routing, or file format.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/set_operation.test` covers mixed distinct and
  duplicate-preserving set operations, bracketed precedence, table value
  constructors, derived tables, views, prepared execution, `EXPLAIN EXTENDED`,
  large `UNION ALL` / `EXCEPT ALL` inputs, and an empty-intermediate-result
  regression under `tmp_memory_table_size=0`.
- The selected test exercises views as part of the MTR smoke profile. That is
  useful upstream baseline evidence, but does not change the default MyLite
  embedded profile's documented disabled view runtime.
- `mariadb/sql/sql_yacc.yy` parses `UNION`, `EXCEPT`, and `INTERSECT`
  set-operation tokens and maps set-operation options onto `EXCEPT_TYPE`.
- `mariadb/sql/sql_union.cc` implements `st_select_lex_unit` preparation,
  optimization, and execution for distinct and duplicate-preserving set
  operations.
- Probe evidence before normalization:
  `tools/mylite-mtr-harness probe main.set_operation` failed only at explicit
  `engine=MyISAM` scratch tables because native MyISAM is deliberately
  unavailable in the smoke profile.

## Design

- Substitute the test's scratch `engine=MyISAM` tables with `engine=Aria`, the
  MTR smoke profile's available persistent engine.
- Add narrow `--replace_result` directives around the substituted DDL so the
  upstream expected result file remains unchanged.
- Add `main.set_operation` beside the existing `UNION`, `EXCEPT`, and
  `INTERSECT` smoke tests in the curated harness list.
- Scope docs to selected mixed set-operation behavior.

## Compatibility Impact

The curated MTR smoke runner gains upstream embedded baseline coverage for
mixed SQL set-operation behavior. This is MariaDB embedded compatibility
evidence, not a new MyLite routed-storage claim.

## Single-File And Lifecycle Impact

No MyLite `.mylite` file lifecycle change. The test runs inside the MTR smoke
vardir with Aria files owned by the MTR run.

## Public API And File-Format Impact

No public API or durable MyLite file-format change.

## Storage-Engine Routing Impact

No MyLite routing change. The test validates MariaDB embedded set-operation
behavior under Aria-backed MTR scratch tables.

## Build, Size, And Dependencies

No production dependency, binary-size, or default-build change.

## Test Plan

- `tools/mylite-mtr-harness probe main.set_operation`
- `tools/mylite-mtr-harness run main.set_operation`
- `tools/mylite-mtr-harness run`
- `tools/mylite-mtr-harness list | wc -l`
- `bash -n tools/mylite-mtr-harness`
- `find . -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- `main.set_operation` reports an MTR pass under strict harness execution.
- The full curated MTR smoke suite remains green.
- No `.reject` files remain.
- Docs scope the claim to selected mixed set-operation behavior.

## Verification Results

- `tools/mylite-mtr-harness probe main.set_operation`: failed before
  normalization at explicit native-MyISAM scratch tables, then passed after
  Aria substitution.
- `tools/mylite-mtr-harness run main.set_operation`: passed.
- `tools/mylite-mtr-harness run`: passed bootstrap plus all 169 selected
  `main` tests.
- `tools/mylite-mtr-harness list | wc -l`: `170`.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find . -name '*.reject' -print`: no output.
- `git diff --check`: passed.

## Risks And Follow-Up

The test uses Aria scratch tables in MTR, not MyLite-routed storage. Future
MyLite routed-storage set-operation claims need first-party storage-smoke
coverage over MyLite handler tables.

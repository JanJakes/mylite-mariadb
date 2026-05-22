# MTR INTERSECT Set-Operation Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.intersect`. This adds
upstream embedded baseline coverage for SQL `INTERSECT`, mixed
`UNION` / `EXCEPT` / `INTERSECT` precedence, derived-table set operations,
prepared set-operation statements, `EXPLAIN` / `ANALYZE` output, BLOB result
materialization, parser errors around `INTO` and `ORDER BY`, and `ANY` /
`EXISTS` subqueries containing `INTERSECT`.

## Non-Goals

- Running MTR against MyLite storage-engine routing.
- Claiming MyLite storage-level set-operation behavior.
- Enabling native MyISAM or changing the MTR smoke profile's storage-engine
  policy.
- Claiming default-product view or stored-program runtime support.
- Changing SQL set-operation semantics, optimizer behavior, storage behavior,
  metadata routing, or file format.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/intersect.test` covers distinct `INTERSECT`
  queries, mixed set-operation precedence, `EXPLAIN` / `ANALYZE`, prepared
  execution, derived set-operation tables, BLOB result materialization through
  CTAS, parser errors, and `ANY` / `EXISTS` subquery regressions.
- The selected test also exercises views and stored procedures as part of the
  MTR smoke profile. That is useful upstream baseline evidence, but does not
  change the default MyLite embedded profile's documented disabled surfaces.
- `mariadb/sql/sql_yacc.yy` parses `EXCEPT` / `INTERSECT` set-operation tokens
  and maps set-operation options onto `EXCEPT_TYPE`.
- `mariadb/sql/sql_union.cc` implements `st_select_lex_unit` preparation,
  optimization, and execution for `UNION`, `EXCEPT`, and `INTERSECT`,
  including distinct set-operation handling.
- Static inspection before admission found explicit `engine=MyISAM` scratch
  tables, which are incompatible with the smoke profile because native MyISAM
  is deliberately unavailable there.

## Design

- Substitute the test's scratch `engine=MyISAM` tables with `engine=Aria`, the
  MTR smoke profile's available persistent engine.
- Add narrow `--replace_result` directives around the substituted DDL and
  `SHOW CREATE TABLE` output so the upstream expected result file remains
  unchanged.
- Add `main.intersect` beside the existing subquery, `UNION`, and
  `EXCEPT` / `EXCEPT ALL` smoke tests in the curated harness list.
- Scope docs to selected `INTERSECT` set-operation behavior.

## Compatibility Impact

The curated MTR smoke runner gains upstream embedded baseline coverage for
selected SQL `INTERSECT` behavior. This is MariaDB embedded compatibility
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

- `tools/mylite-mtr-harness probe main.intersect`
- `tools/mylite-mtr-harness run main.intersect`
- `tools/mylite-mtr-harness run`
- `tools/mylite-mtr-harness list | wc -l`
- `bash -n tools/mylite-mtr-harness`
- `find . -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- `main.intersect` reports an MTR pass under strict harness execution.
- The full curated MTR smoke suite remains green.
- No `.reject` files remain.
- Docs scope the claim to selected `INTERSECT` behavior.

## Verification Results

- `tools/mylite-mtr-harness probe main.intersect`: passed.
- `tools/mylite-mtr-harness run main.intersect`: passed.
- `tools/mylite-mtr-harness run`: passed bootstrap plus all 168 selected
  `main` tests.
- `tools/mylite-mtr-harness list | wc -l`: `169`.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find . -name '*.reject' -print`: no output.
- `git diff --check`: passed.

## Risks And Follow-Up

The test uses Aria scratch tables in MTR, not MyLite-routed storage. Future
MyLite routed-storage set-operation claims need first-party storage-smoke
coverage over MyLite handler tables.

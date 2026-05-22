# MTR EXCEPT Set-Operation Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.except` and
`main.except_all`. This adds upstream embedded baseline coverage for SQL
`EXCEPT`, `EXCEPT ALL`, derived-table set operations, prepared set-operation
statements, `EXPLAIN` / `ANALYZE` output, BLOB result materialization, parser
errors around `INTO` and `ORDER BY`, and `ANY` subqueries containing `EXCEPT`.

## Non-Goals

- Running MTR against MyLite storage-engine routing.
- Claiming MyLite storage-level set-operation behavior.
- Enabling native MyISAM or changing the MTR smoke profile's storage-engine
  policy.
- Changing SQL set-operation semantics, optimizer behavior, storage behavior,
  metadata routing, or file format.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/except.test` covers distinct `EXCEPT` queries,
  `EXPLAIN` / `ANALYZE`, prepared execution, derived set-operation tables,
  BLOB result materialization through CTAS, parser errors, and an `ANY`
  subquery regression.
- `mariadb/mysql-test/main/except_all.test` covers duplicate-preserving
  `EXCEPT ALL`, mixed `UNION ALL` / `EXCEPT` chains, `EXPLAIN` / `ANALYZE`,
  prepared execution, BLOB result materialization through CTAS, parser errors,
  and an `ANY` subquery regression.
- `mariadb/sql/sql_yacc.yy` parses `EXCEPT` / `INTERSECT` set-operation tokens
  and maps set-operation options onto `EXCEPT_TYPE`.
- `mariadb/sql/sql_union.cc` implements `st_select_lex_unit` preparation,
  optimization, and execution for `UNION`, `EXCEPT`, and `INTERSECT`,
  including `EXCEPT_ALL` duplicate-counter behavior.
- Probe evidence before admission:
  `tools/mylite-mtr-harness probe main.except main.except_all` failed only at
  explicit `engine=MyISAM` scratch tables because native MyISAM is deliberately
  unavailable in the smoke profile.

## Design

- Substitute the tests' scratch `engine=MyISAM` tables with `engine=Aria`, the
  MTR smoke profile's available persistent engine.
- Add narrow `--replace_result` directives around the substituted DDL and
  `SHOW CREATE TABLE` output so the upstream expected result files remain
  unchanged.
- Add `main.except` and `main.except_all` beside the existing subquery/UNION
  smoke tests in the curated harness list.
- Scope docs to selected `EXCEPT` / `EXCEPT ALL` set-operation behavior.

## Compatibility Impact

The curated MTR smoke runner gains upstream embedded baseline coverage for
selected SQL set-operation behavior beyond `UNION`. This is MariaDB embedded
compatibility evidence, not a new MyLite routed-storage claim.

## Single-File And Lifecycle Impact

No MyLite `.mylite` file lifecycle change. The tests run inside the MTR smoke
vardir with Aria files owned by the MTR run.

## Public API And File-Format Impact

No public API or durable MyLite file-format change.

## Storage-Engine Routing Impact

No MyLite routing change. The tests validate MariaDB embedded set-operation
behavior under Aria-backed MTR scratch tables.

## Build, Size, And Dependencies

No production dependency, binary-size, or default-build change.

## Test Plan

- `tools/mylite-mtr-harness probe main.except main.except_all`
- `tools/mylite-mtr-harness run main.except main.except_all`
- `tools/mylite-mtr-harness run`
- `tools/mylite-mtr-harness list | wc -l`
- `bash -n tools/mylite-mtr-harness`
- `find . -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- Both selected tests report MTR passes under strict harness execution.
- The full curated MTR smoke suite remains green.
- No `.reject` files remain.
- Docs scope the claim to selected `EXCEPT` / `EXCEPT ALL` behavior.

## Verification Results

- `tools/mylite-mtr-harness probe main.except main.except_all`: passed.
- `tools/mylite-mtr-harness run main.except main.except_all`: passed.
- `tools/mylite-mtr-harness run`: passed bootstrap plus all 167 selected
  `main` tests.
- `tools/mylite-mtr-harness list | wc -l`: `168`.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find . -name '*.reject' -print`: no output.
- `git diff --check`: passed.

## Risks And Follow-Up

The tests use Aria scratch tables in MTR, not MyLite-routed storage. Future
MyLite routed-storage set-operation claims need first-party storage-smoke
coverage over MyLite handler tables.

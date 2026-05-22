# MTR Negation-Elimination Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with
`main.negation_elimination`. This adds upstream embedded baseline coverage for
boolean `NOT` simplification, comparison negation, `IS NULL` / `IS NOT NULL`
negation, compound `AND` / `OR` negation, XOR negation, and representative
range-plan output for those predicates.

## Non-Goals

- Running MTR against MyLite storage-engine routing.
- Claiming MyLite storage-level optimizer behavior.
- Changing SQL optimizer behavior, storage behavior, metadata routing, or file
  format.
- Enabling native MyISAM, InnoDB, partitioning, Sequence-engine, debug-only, or
  server-only test surfaces.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/negation_elimination.test` creates one ordinary
  indexed table and exercises boolean negation simplification through `SELECT`
  results and `EXPLAIN` / `EXPLAIN EXTENDED` output.
- `mariadb/sql/sql_parse.cc::negate_expression()` handles double negation and
  delegates other forms to `Item::neg_transformer()`.
- `mariadb/sql/item_cmpfunc.cc` implements predicate negation for `NOT`,
  comparison predicates, XOR, `IS NULL` / `IS NOT NULL`, and `AND` / `OR`
  boolean conditions.
- `mariadb/sql/sql_select.cc::remove_eq_conds()` participates in later
  condition simplification before range planning.
- Probe evidence before normalization:
  `tools/mylite-mtr-harness probe main.negation_elimination` reached only
  Aria row-estimate differences in selected `EXPLAIN` rows. Result sets and
  rewritten predicates matched the upstream expectation.

## Design

- Add `main.negation_elimination` beside existing parser/expression predicate
  smoke coverage.
- Keep the upstream expected result file intact.
- Add narrow `--replace_column` directives before row-estimate-sensitive
  `EXPLAIN` statements so the smoke profile's Aria estimate differences do not
  mask the semantic negation checks.
- Scope docs to selected negation-elimination predicate optimizer behavior.

## Compatibility Impact

The curated MTR smoke runner gains upstream embedded baseline coverage for
selected predicate negation behavior. This is MariaDB embedded compatibility
evidence, not a new MyLite routed-storage claim.

## Single-File And Lifecycle Impact

No MyLite `.mylite` file lifecycle change. The test runs inside the MTR smoke
vardir with Aria as the available persistent engine.

## Public API And File-Format Impact

No public API or durable MyLite file-format change.

## Storage-Engine Routing Impact

No MyLite routing change. The test validates MariaDB embedded predicate
rewriting and range-planning behavior under the smoke profile.

## Build, Size, And Dependencies

No production dependency, binary-size, or default-build change.

## Test Plan

- `tools/mylite-mtr-harness probe main.negation_elimination`
- `tools/mylite-mtr-harness run main.negation_elimination`
- `tools/mylite-mtr-harness run`
- `tools/mylite-mtr-harness list | wc -l`
- `bash -n tools/mylite-mtr-harness`
- `find . -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- `main.negation_elimination` reports an MTR pass under strict harness
  execution.
- The full curated MTR smoke suite remains green.
- No `.reject` files remain.
- Docs scope the claim to selected negation-elimination predicate behavior.

## Verification Results

- `tools/mylite-mtr-harness probe main.negation_elimination`: passed.
- `tools/mylite-mtr-harness run main.negation_elimination`: passed.
- `tools/mylite-mtr-harness run`: passed bootstrap plus all 165 selected
  `main` tests.
- `tools/mylite-mtr-harness list | wc -l`: `166`.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find . -name '*.reject' -print`: no output.
- `git diff --check`: passed.

## Risks And Follow-Up

The selected test remains optimizer-plan sensitive. Future MyLite routed-storage
optimizer claims need first-party storage-smoke coverage over MyLite handler
tables instead of relying on this MariaDB embedded baseline.

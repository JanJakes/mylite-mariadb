# Prepared Update Simple Value Setup Elision

## Problem

The routed prepared primary-key update loop still reparses part of MariaDB's
single-table `UPDATE` setup before reaching the accepted MyLite direct-update
path. A current local sample shows time in `Sql_cmd_update::prepare_inner()`,
`JOIN::prepare()`, `open_tables_for_query()`, and the value-list
`setup_fields()` call used by MyLite's single-update result-elision path.

Prepared updates that assign a literal or bound scalar parameter, such as
`SET value = ?`, cannot read table columns, introduce subqueries, or require
read-map registration for the assigned value. Running the full value
`setup_fields()` pass for that shape is therefore empty work.

The current prepared-update component benchmark uses
`SET value = value + 1 WHERE id = ?`, so it intentionally remains on the
normal value setup path. This slice does not claim to remove that benchmark's
expression setup cost.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_update.cc::Sql_cmd_update::prepare_inner()` calls
  `mylite_prepare_single_update_values()` for MyLite's elided single-table
  result path.
- `mylite_prepare_single_update_values()` currently runs `setup_fields()` over
  every update value and then checks explicit field assignability.
- `mariadb/sql/table.cc::TABLE::check_assignability_explicit_fields()` checks
  each value against the target field independently of read-map registration.
- `mariadb/sql/item.h::Item::basic_const_item()` identifies literal-like
  constants. Bound scalar parameter markers enter this state after execution
  binding when they are safe to treat like literals for read-map setup.
- `mariadb/sql/item.cc::Item_param::is_evaluable_expression()` rejects
  `DEFAULT` and `IGNORE` markers, while `Item_param::basic_const_item()` stays
  false for unbound `NO_VALUE` markers. Combining the two keeps parameter
  skipping limited to assigned literal-like values.
- Non-literal expressions, field references, default/contextual values,
  functions, and subqueries can still need normal value setup and must stay on
  the existing path.

## Design

- Add a narrow value-list predicate before MyLite's single-update value setup
  pass. Keep it cheap enough to run on every execution so parameter marker
  state changes cannot reuse a stale decision.
- Treat only basic literal-like values, including bound scalar parameters that
  report `basic_const_item()` after a value is assigned, as safe to skip.
- Continue to run `setup_fields()` for any function, field reference,
  contextual/default value, non-basic expression, or subquery.
- Always keep `TABLE::check_assignability_explicit_fields()` so bound values are
  still checked against the target column type on every execution.

## Compatibility Impact

No SQL-visible behavior change is intended. Ordinary parameter/literal updates
skip only read-map and field-fixup work that cannot affect their value list.
Expression updates such as `SET qty = qty + ?`, generated-column dependencies,
CHECK constraints, FK checks, warnings, strict conversion errors, and fallback
update shapes continue through the existing setup path.

## Single-File And Lifecycle Impact

No storage format, journal, checkpoint, sidecar, lock, or file lifecycle
change.

## Public API And File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Test Plan

- Rebuild the MariaDB embedded archive with static MyLite storage.
- Build `mylite_embedded_statement_test`,
  `mylite_embedded_storage_engine_test`, and `mylite_perf_baseline`.
- Run focused prepared-statement and embedded storage-engine tests.
- Run full `ctest --preset storage-smoke-dev --output-on-failure`.
- Run `mylite_perf_baseline --phase=prepared-update-components 10000 1000000`
  to verify the expression-update benchmark does not materially regress.
- Keep expression-based prepared update coverage passing so the fallback setup
  path remains covered.

## Acceptance Criteria

- Prepared parameter/literal updates skip the value `setup_fields()` pass in
  MyLite's single-update result-elision path.
- Prepared expression updates still execute the normal value setup path.
- Existing prepared DML semantics, diagnostics, statement effects, rollback,
  CHECK, generated-column, and FK coverage pass.
- The local expression-based prepared-update step component does not materially
  regress.

## Risks

- MariaDB item cleanup can leave parameter markers with changing fixed state
  between executions. The skip predicate must not cache parameter state across
  executions.
- Over-broad skipping could miss read-map registration for `SET` expressions
  that reference table columns. The first slice therefore accepts only
  evaluable basic constants.

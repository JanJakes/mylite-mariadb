# MyLite Single Update Result Elision

## Problem

Prepared primary-key `UPDATE` samples remain dominated by repeated
`Sql_cmd_dml::prepare()` work. Part of that work constructs and prepares a
`multi_update` result helper even when the statement is an ordinary
single-table MyLite update that later executes through
`Sql_cmd_update::update_single_table()`.

For this path, the multi-table result helper is not used to execute rows or
report statement effects. The single-table path owns row mutation and updates
`Sql_cmd_update::found` / `updated` directly.

## Source Findings

- MariaDB base line: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_update.cc::Sql_cmd_update::prepare_inner()` allocates
  `multi_update`, calls `multi_update::init()`, then passes it to
  `JOIN::prepare()` for both single-table and multi-table UPDATE.
- `mariadb/sql/sql_update.cc::Sql_cmd_update::execute_inner()` dispatches
  non-multitable statements to `update_single_table()`, then deletes `result`
  only if it exists. Statement effects come from `Sql_cmd_update` fields, not
  from `multi_update`, in the single-table case.
- `multi_update::prepare()` still performs value expression setup,
  assignability checks, target-field association, and multi-table-only list and
  copy-field setup.
- `update_single_table()` already applies trigger nullable-field switching for
  the base fields and values before executing rows.

## Design

1. Add a MyLite-only single-table prepare helper that applies the value
   expression setup and assignability checks needed when `JOIN::prepare()` runs
   without a result helper.
2. For ordinary MyLite single-table base-table UPDATE, pass `NULL` as the JOIN
   result and skip `multi_update` allocation, `multi_update::init()`, and
   `multi_update::prepare()`.
3. Keep the existing target-field association loop in
   `Sql_cmd_update::prepare_inner()` for all paths.
4. Leave multi-table UPDATE, views or derived tables, period updates,
   `RETURNING`, SET expressions with subqueries, non-MyLite execution, and any
   path that needs the multi-table result helper on the existing MariaDB path.

This keeps normal table opening, locking, JOIN condition setup, expression
fixing for the WHERE path, range planning, handler direct-update eligibility,
row mutation, diagnostics, and statement-effect reporting.

## Affected MariaDB Subsystems

- Single-table `UPDATE` prepare in `mariadb/sql/sql_update.cc`.

No parser, handler, storage, catalog, file-format, or wire-protocol behavior
changes.

## Compatibility Impact

Ordinary MyLite single-table base-table UPDATE should preserve SQL behavior and
diagnostics. Unsupported or broader shapes keep the existing `multi_update`
prepare helper.

Explicit `EXPLAIN UPDATE` remains compatible because the JOIN still prepares
the WHERE/table plan, and `update_single_table()` owns the explain output path.

## DDL Metadata Routing Impact

No DDL metadata routing changes.

## Single-File And Embedded Lifecycle Impact

No durable state, sidecar, locking, recovery, or handle-lifecycle changes.

## Public API Or File-Format Impact

No public `libmylite` API or `.mylite` file-format changes.

## Storage-Engine Routing Impact

No routing-policy change. The optimization applies after SQL parsing has
selected single-table UPDATE and only under MyLite schema hooks.

## Wire-Protocol Or Integration-Package Impact

No wire-protocol behavior changes.

## Binary-Size Impact

The slice adds a small helper and branch. It removes work from the hot path but
does not remove compiled code or add dependencies. Archive-size impact should
be neutral to negligible and measured through the storage-smoke embedded
archive rebuild.

## Test And Verification Plan

- Rebuild the storage-smoke MariaDB embedded archive with static MyLite
  storage.
- Build storage-smoke embedded storage-engine and performance targets.
- Run focused storage-smoke capability, embedded comparison, and embedded
  storage-engine tests.
- Run prepared-update component and full prepared-update performance baselines.
- Sample the prepared-update component phase and confirm the accepted direct
  path no longer reports `multi_update::init()` or `multi_update::prepare()`.
- Reuse existing routed explicit `EXPLAIN UPDATE` and prepared UPDATE effect
  coverage.
- Run `git diff --check` and `git clang-format --diff` on touched C/C++ files.

## Acceptance Criteria

- Focused tests pass.
- Prepared point updates preserve affected-row, no-match, unchanged-row, and
  warning-count behavior.
- Explicit routed `EXPLAIN UPDATE` remains covered and keeps expected key
  output.
- The hot prepared-update sample avoids `multi_update::init()` and
  `multi_update::prepare()` on the accepted single-table MyLite path.

## Risks And Unresolved Questions

- `multi_update::prepare()` does more than validate assignments for multi-table
  execution. The elision must therefore stay limited to single-table base-table
  updates without nested SET subqueries where `update_single_table()` performs
  execution.
- This does not solve repeated JOIN condition setup or table opening. Those
  need separate designs with stronger metadata and table-pointer invariants.

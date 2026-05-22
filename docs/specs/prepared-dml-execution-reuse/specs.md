# Prepared DML Execution Reuse

## Problem

The storage mutation component for indexed prepared updates is now about
0.25 us/op locally, while the full prepared update step is about 1.7 us/op.
Focused samples show the remaining cost dominated by MariaDB SQL-layer work,
especially:

- `open_tables_for_query()` and MDL/table-cache lookup,
- `Sql_cmd_update::prepare_inner()`,
- `JOIN::prepare()`,
- repeated setup and validation around the accepted MyLite direct-update path.

This is the next major barrier to SQLite-like prepared statement performance.
Small storage rewrites will not close most of the remaining gap.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_prepare.cc::Prepared_statement::execute_loop()` binds
  parameters, installs a reprepare observer for fragile statements, and calls
  `Prepared_statement::execute()`.
- `mariadb/sql/sql_prepare.cc::Prepared_statement::execute()` calls
  `reinit_stmt_before_use()`, then `mysql_execute_command(thd, true)`.
- `mariadb/sql/sql_select.cc::Sql_cmd_dml::execute()` currently prepares DML
  commands when `!is_prepared()`, opens tables through
  `open_tables_for_query()`, locks tables, executes the command, calls
  `unit->cleanup()`, and then calls `unprepare(thd)`.
- The `Sql_cmd_dml::execute()` source comment explicitly notes that the
  already-prepared DML branch is currently never used because
  `unit->cleanup()` effectively unprepares DML command objects.
- `mariadb/sql/sql_update.cc::Sql_cmd_update::prepare_inner()` builds a new
  `JOIN`, runs `setup_tables()`, `JOIN::prepare()`, and then sets up
  assignment/value associations.
- `mariadb/sql/sql_update.cc::Sql_cmd_update::update_single_table()` contains
  the accepted MyLite direct-update path, but that path is reached only after
  the repeated prepare/open/setup work.
- MyLite has already narrowed the accepted hot update shape through
  `Mylite_update_exact_key_proof_cache`, handler direct-update proof pushdown,
  direct-update explain gating, and direct handler update execution.
- The no-match exact-key prepared update path reaches the same repeated
  table-open, prepare, lock, and handler lookup layers, but does not mutate a
  row. It is useful as a diagnostic boundary before changing DML cleanup or
  attempting `JOIN` reuse.

## Design Direction

Do not skip MariaDB DML preparation wholesale. The current prepare path owns
metadata validation, table opening, privilege checks, name resolution, field
fixup, expression setup, reprepare detection, lock acquisition, and fallback
behavior. A safe reuse design must split those responsibilities deliberately.

The staged design should be:

1. Define a narrow MyLite prepared direct-update eligibility predicate.
   It should initially cover one file-backed MyLite table, no trigger/runtime
   view/period/versioned behavior, no `RETURNING`, no subqueries, no
   multi-table update, no `ORDER BY` / `LIMIT`, no `IGNORE`, and an exact
   non-null single-part unique-key predicate already accepted by the MyLite
   SQL-layer proof cache.
2. Keep metadata validation and table locking until a separate table-handle
   lifetime design proves it can retain open tables safely across prepared
   executions.
3. Split DML cleanup from DML unprepare so a prepared command can keep reusable
   shape state only after normal execution cleanup remains correct.
4. Cache only immutable shape decisions first: direct-update eligibility,
   value-list has no subquery, no explain-observable path, and the exact-key
   proof metadata. Do not cache runtime `Item` values, row buffers, handler
   state, diagnostics, warning state, or table locks.
5. Once shape reuse is proven, evaluate whether the accepted path can bypass
   `JOIN::prepare()` while still revalidating opened table metadata and
   reconnecting field/table pointers after `reinit_stmt_before_use()`.
6. Treat cross-execution open-table or MDL reuse as a later slice. It must
   integrate with MariaDB reprepare invalidation, temporary table shadowing,
   DDL invalidation, active transactions, and MyLite single-file lifetime.

## Initial Implementation Slice

The first code slice should not retain table handles. It should introduce
explicit prepared-update reuse state and prove that normal execution cleanup can
be separated from unprepare for an eligible MyLite direct-update shape without
changing behavior. If that cannot be done cleanly, stop at a measured
instrumentation slice instead of bypassing core MariaDB checks.

The current implementation proceeds with that instrumentation boundary first:
`prepared-row-only-update-miss-components` binds out-of-range primary-key
values against the row-only update table and records bind, step, and reset
components. The phase keeps the same single-table prepared update SQL shape as
`prepared-row-only-update-components`, but the step returns no matching row, so
it separates table-open, DML prepare, lock, exact lookup, and reset cost from
row materialization and storage mutation cost.

Candidate acceptance for the first implementation:

- Prepared exact-key updates keep affected-row, no-match, unchanged-row,
  warning, strict-mode, generated-column, CHECK, FK, and rollback behavior.
- Unsupported update shapes still run through the existing DML prepare path.
- Reprepare and metadata invalidation still force the normal prepare path.
- Explicit `EXPLAIN UPDATE`, `ANALYZE UPDATE`, slow-log explain/engine output,
  triggers, views, period tables, versioned tables, subqueries, `RETURNING`,
  `ORDER BY`, `LIMIT`, `IGNORE`, and multi-table updates stay out of the reuse
  path.
- The local prepared-update sample shows a material reduction in
  `JOIN::prepare()` before claiming success.
- The no-match component benchmark records a zero row-only checksum, proving
  that the phase did not mutate rows while measuring the prepared-DML path.

## Affected Subsystems

- MariaDB prepared statement execution in `sql_prepare.cc`.
- DML command prepare/execute cleanup in `sql_select.cc`.
- Single-table update prepare and direct-update execution in `sql_update.cc`.
- MyLite handler direct-update routing in `storage/mylite/ha_mylite.cc`.
- `libmylite` prepared row-DML performance.

## Compatibility Impact

The goal is no SQL-visible behavior change. MariaDB must remain authoritative
for name resolution, type conversion, expression evaluation, diagnostics,
warnings, affected rows, row counts, generated columns, CHECK constraints,
foreign keys, and transaction behavior.

Any reuse path must be disabled when table metadata changes or when MariaDB's
reprepare observer would require a full prepare.

## Single-File And Embedded Lifecycle Impact

No `.mylite` file-format change is required. The slice must preserve the
single-file storage model and must not introduce persistent sidecars. Table
handle reuse, if attempted later, must be scoped to an open `libmylite`
database handle and must not outlive that handle's embedded runtime.

## Public API And File-Format Impact

No public `libmylite` API or `.mylite` file-format change is planned.

## Binary-Size And Dependency Impact

No new dependency. The code should remain a narrow MariaDB fork delta; binary
size impact should be measured if new reusable command-state structures are
added.

## Tests And Verification

- Rebuild the storage-smoke MariaDB embedded archive with static MyLite
  storage.
- Build storage-smoke embedded statement, storage-engine, comparison, and
  performance targets.
- Run focused prepared statement and routed storage-engine tests.
- Run full `ctest --preset storage-smoke-dev --output-on-failure`.
- Run `mylite_perf_baseline --phase=prepared-update-components 1000 1000000`.
- Run `mylite_perf_baseline --phase=prepared-row-only-update-miss-components
  10000 1000000` to quantify no-match prepared update overhead.
- Capture a focused prepared-update sample and compare `open_tables_for_query`,
  `Sql_cmd_update::prepare_inner()`, and `JOIN::prepare()` against the current
  baseline.
- Add regression tests for unsupported-shape fallback before enabling any
  bypass.
- Current instrumentation slice verification:
  - Passed `git clang-format --diff -- tools/mylite_perf_baseline.c`.
  - Passed `git diff --check`.
  - Passed `cmake --build --preset storage-smoke-dev --target
    mylite_perf_baseline`.
  - Passed `ctest --preset storage-smoke-dev --output-on-failure`.
  - Ran `build/storage-smoke-dev/tools/mylite_perf_baseline
    --phase=prepared-row-only-update-miss-components 10000 1000000`:
    - bind: `0.021 us/op`
    - step: `1.134 us/op`
    - reset: `0.023 us/op`
    - row-only miss checksum: `0`

## Risks And Unresolved Questions

- `unit->cleanup()` and DML `unprepare()` are entangled today. Separating them
  without leaks or stale pointers is the primary risk.
- Reusing a prepared update shape before table metadata is revalidated could
  break DDL invalidation, temporary table shadowing, or view/trigger behavior.
- Skipping `JOIN::prepare()` may miss field fixups or expression rewrites that
  are still needed after parameter rebinding.
- Retaining open tables or MDL tickets across executions is a separate, higher
  risk design and should not be mixed into the first reuse slice.

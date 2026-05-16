# Optimizer Trace Trim

## Problem

The default embedded profile still builds MariaDB's optimizer trace runtime.
Optimizer trace records per-session JSON diagnostics for optimizer decisions
and exposes them through `INFORMATION_SCHEMA.OPTIMIZER_TRACE`. This is a query
diagnostics surface, not durable application state and not part of MyLite's
file-owned embedded API contract.

## Source Findings

- Base: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- MariaDB documents optimizer trace as a JSON document for optimizer decisions
  on traceable `SELECT`, `UPDATE`, and `DELETE` statements, enabled through
  the `optimizer_trace` system variable:
  <https://mariadb.com/kb/en/optimizer-trace-overview/>.
- MariaDB documents `INFORMATION_SCHEMA.OPTIMIZER_TRACE` as the table that
  contains optimizer trace information for the last executed query:
  <https://mariadb.com/docs/server/reference/system-tables/information-schema/information-schema-tables/information-schema-optimizer_trace-table>.
- `mariadb/sql/opt_trace.cc` implements optimizer-trace lifecycle, JSON
  helpers, security checks, `INFORMATION_SCHEMA.OPTIMIZER_TRACE` fields, and
  the schema-table fill path.
- `mariadb/sql/sys_vars.cc` registers `optimizer_trace` and
  `optimizer_trace_max_mem_size` session/global variables.
- `mariadb/sql/sql_parse.cc`, `mariadb/sql/sql_prepare.cc`,
  `mariadb/sql/sp_instr.cc`, and optimizer helper sources instantiate
  `Opt_trace_start` or use trace JSON helpers from ordinary statement paths.
- The trace helper symbols are widely referenced by retained optimizer and
  parser code, so the safe embedded cut is an inert replacement object rather
  than deleting the header-level API.

## Design

- Add `MYLITE_WITH_OPTIMIZER_TRACE`, defaulting to `ON` for upstream-style
  builds and forced `OFF` in `cmake/mariadb-embedded-baseline.cmake`.
- Replace `opt_trace.cc` with `mylite_opt_trace_disabled.cc` when disabled.
  The stub keeps required field metadata and helper symbols, but never starts,
  stores, fills, or exposes optimizer trace rows.
- Add public MyLite SQL-policy rejection for:
  - assignments to `optimizer_trace`,
  - assignments to `optimizer_trace_max_mem_size`,
  - references to `INFORMATION_SCHEMA.OPTIMIZER_TRACE`.
- Preserve ordinary SQL planning and execution. `EXPLAIN` and optimizer code
  paths can still instantiate trace helper wrappers, but those wrappers are
  inert in the embedded profile.

## Compatibility Impact

Optimizer trace becomes explicitly unsupported. This removes diagnostic JSON
for optimizer internals, not query execution, DDL metadata, row storage, or
query planning behavior. Applications should not rely on MyLite for
server-style optimizer trace inspection.

## Single-File And Embedded Lifecycle Impact

No file-format change. The trim removes per-session optimizer trace collection
from embedded execution and does not add sidecars, durable metadata, or
transient companions.

## Storage-Engine Routing Impact

No storage-routing change. Optimizer trace observes planner decisions only;
routed tables, indexes, and MyLite storage remain unchanged.

## Public API Impact

No C API surface change. `mylite_exec()` and `mylite_prepare()` return stable
unsupported-surface diagnostics for optimizer-trace SQL.

## Binary-Size Impact

The measured size impact is archive-side and small because retained optimizer
code still references trace helper symbols. The default embedded archive is
11,056 bytes smaller with the same member count, and the storage-smoke archive
is also 11,056 bytes smaller with the same member count.

## License And Dependency Impact

No new dependency and no license change. The slice uses a MyLite GPL-compatible
disabled stub in the MariaDB-derived source tree.

## Test And Verification Plan

- Add direct SQL tests for rejected optimizer-trace system-variable assignment
  and rejected `INFORMATION_SCHEMA.OPTIMIZER_TRACE` access.
- Add prepared-statement rejection tests for representative optimizer-trace
  SQL.
- Run default and storage-smoke MariaDB embedded builds and measurements.
- Verify `MYLITE_WITH_OPTIMIZER_TRACE:BOOL=OFF` appears in measured cache
  options and `mylite_opt_trace_disabled.cc.o` replaces `opt_trace.cc.o`.
- Run `embedded-dev`, `storage-smoke-dev`, and `dev` CMake build/test presets.
- Run the server-surface compatibility harness, size report, formatting, tidy,
  shell syntax checks, and diff checks.

## Acceptance Criteria

- The disabled embedded profile compiles with
  `MYLITE_WITH_OPTIMIZER_TRACE=OFF`.
- Direct and prepared optimizer-trace SQL is rejected with MyLite diagnostics.
- Ordinary SQL planning and execution remain unaffected.
- Size measurements and architecture/compatibility docs are updated.

## Risks

- Optimizer trace helper objects are included in ordinary optimizer code paths,
  so the disabled object must preserve helper symbols as inert operations.
- MariaDB still registers optimizer-trace system variables. MyLite policy must
  reject writes before users can enable a no-op diagnostic surface.
- `INFORMATION_SCHEMA.OPTIMIZER_TRACE` metadata remains registered in
  `sql_show.cc`; MyLite policy must reject table access to keep unsupported
  diagnostics explicit.

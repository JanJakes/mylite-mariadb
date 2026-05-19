# Prepared Parameter Bind Reuse

## Problem

Prepared point-select loops still rebuild and rebind MariaDB parameter buffers
on every execution. The local baseline's hot path binds one integer parameter,
steps one row, drains the statement, resets, and binds another integer. MyLite
currently clears bindings during `mylite_reset()`, so every loop iteration
forces a new `mysql_stmt_bind_param()` call even when the parameter layout is
unchanged.

That behavior is also less SQLite-like than the rest of the `libmylite`
prepared API: SQLite retains bindings across `sqlite3_reset()` and exposes an
explicit `sqlite3_clear_bindings()` for clearing them.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/libmysqld/libmysql.c::mysql_stmt_bind_param()` copies the bind
  array into statement-owned storage and documents that callers can repeatedly
  change bound variable values and execute without calling bind again unless
  parameter typecodes or other `MYSQL_BIND` members change.
- `mariadb/libmysqld/libmysql.c::mysql_stmt_reset()` calls
  `reset_stmt_handle()` with server-side reset and buffer-flush flags; the reset
  helper does not clear the statement-owned parameter bind array.
- `packages/libmylite/src/database.cc::mylite_reset()` currently calls
  `reset_statement_bindings()`, making all parameters NULL on reset.
- `packages/libmylite/src/database.cc::bind_statement_parameters()` rebuilds
  `statement.parameter_binds` and calls `mysql_stmt_bind_param()` before every
  execution.

## Design

- Change `mylite_reset()` so it resets statement execution state but preserves
  current parameter bindings.
- Keep `mylite_clear_bindings()` as the explicit API for clearing bound
  parameters and releasing custom or borrowed-value ownership.
- Update binding lifetime docs:
  - `MYLITE_STATIC` data must remain valid until the value is rebound,
    `mylite_clear_bindings()` is called, or the statement is finalized;
  - `mylite_reset()` no longer releases bound values.
- Add statement-level parameter bind cache state:
  - whether the MariaDB parameter bind array has been installed;
  - whether the bind layout is dirty and must be rebound.
- For scalar bindings, update the existing `BoundValue` storage in place. If
  the parameter kind remains the same scalar kind, do not mark the bind layout
  dirty because MariaDB's copied `MYSQL_BIND` still points at the same storage.
- Mark the bind layout dirty for NULL transitions, scalar-kind changes, and all
  text/BLOB binds because their type, pointer, destructor, or backing storage
  can change.
- On execution, call `mysql_stmt_bind_param()` only when the bind array has not
  been installed yet or the layout is dirty.

## Compatibility Impact

This changes public `libmylite` prepared-statement binding semantics. The new
behavior is intentionally closer to SQLite:

- `mylite_reset()` preserves bindings;
- `mylite_clear_bindings()` clears bindings.

Callers that relied on reset to clear borrowed `MYLITE_STATIC` values must now
clear or rebind explicitly. The project is still early-development, and this
change improves both API ergonomics and hot-loop performance.

## Single-File And Lifecycle Impact

No file-format, storage, sidecar, or transaction lifecycle change.

## Public API And File-Format Impact

No new C API function and no `.mylite` format change. Existing public API
semantics for `mylite_reset()`, `mylite_clear_bindings()`, and bound-value
lifetimes are updated.

## Storage-Engine Routing Impact

The optimization sits above MariaDB execution and storage-engine routing. It
benefits prepared statements over routed MyLite tables through the existing
handler path.

## Binary-Size And Dependency Impact

No new dependency. Binary-size impact is limited to small state fields and
binding-state checks in `libmylite`.

## Test And Verification Plan

- Update prepared statement tests to prove:
  - bindings survive `mylite_reset()` and can execute again without rebinding;
  - `mylite_clear_bindings()` is still the explicit clearing operation;
  - rebinding the same scalar kind across reset works;
  - text/BLOB rebinding still works and custom destructors fire on explicit
    clear/rebind/finalize.
- Run `mylite_embedded_statement_test`.
- Run routed storage-engine compatibility smoke.
- Run the local performance baseline with default and higher iteration counts.
- Run `git diff --check`.

## Acceptance Criteria

- `mylite_reset()` no longer clears bound parameters.
- Hot scalar prepared re-execution avoids repeated `mysql_stmt_bind_param()`
  calls when parameter layouts are unchanged.
- Existing prepared statement, routed storage, and benchmark checksum coverage
  pass.

## Risks

- The binding lifetime change is observable to public API callers. The docs and
  tests must make the new reset-vs-clear split explicit.
- Text/BLOB bind reuse remains conservative in this slice; repeated large-value
  loops can be optimized later with per-parameter pointer and length tracking.

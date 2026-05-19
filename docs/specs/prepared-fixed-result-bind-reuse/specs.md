# Prepared Fixed Result Bind Reuse

## Problem

The current `libmylite` prepared-statement path rebuilds and rebinds result
buffers every time a result-producing prepared statement is executed. Hot
primary-key point selects in the performance baseline fetch a single fixed-width
integer column, so this work repeats even though the prepared statement result
metadata and destination buffers are stable.

`mylite_reset()` also calls `mysql_stmt_free_result()` after fully drained
result sets, even though `mylite_step()` already frees the MariaDB result when
it observes `MYSQL_NO_DATA`.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/libmysqld/libmysql.c::mysql_stmt_bind_result()` copies the provided
  bind array into statement-owned storage allocated during prepare.
- `mariadb/libmysqld/libmysql.c::mysql_stmt_free_result()` calls
  `reset_stmt_handle()` with `RESET_LONG_DATA | RESET_STORE_RESULT |
  RESET_CLEAR_ERROR`, freeing client-side stored result state without clearing
  result bind definitions.
- `mariadb/libmysqld/libmysql.c::mysql_stmt_reset()` uses
  `RESET_SERVER_SIDE | RESET_LONG_DATA | RESET_ALL_BUFFERS |
  RESET_CLEAR_ERROR`; the reset helper does not clear the statement-owned bind
  arrays.
- `packages/libmylite/src/database.cc::bind_statement_results()` allocates and
  fills `statement.result_binds` for every prepared execution, then calls
  `mysql_stmt_bind_result()` before the first fetch.
- `packages/libmylite/src/database.cc::fetch_statement_row()` calls
  `mysql_stmt_free_result()` when `mysql_stmt_fetch()` returns
  `MYSQL_NO_DATA`; `mylite_reset()` calls it again for any executed statement.

## Design

- Track whether a prepared statement currently has an active MariaDB result
  that still needs `mysql_stmt_free_result()`.
- Set the active-result flag after a result-producing `mysql_stmt_execute()`
  succeeds.
- Clear the flag after `fetch_statement_row()` frees a fully drained result and
  after `mylite_reset()` frees an early-abandoned result.
- Reuse result binds only for fixed-width result sets. Variable text and BLOB
  result sets retain the existing per-execution bind path because their backing
  buffers can reallocate during truncation handling.
- Initialize the fixed-width result `MYSQL_BIND` array once, call
  `mysql_stmt_bind_result()` once, and on later executions only reset the
  column value state before fetching.

## Compatibility Impact

No SQL or public API behavior changes. Result-column values, truncation
handling, warnings, reset, and finalize behavior remain the same. This slice
does not change MyLite's current documented behavior that `mylite_reset()`
clears parameter bindings.

## Single-File And Lifecycle Impact

No file-format, storage, sidecar, or transaction lifecycle change.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

The optimization sits above MariaDB execution and storage-engine routing. It
applies equally to routed MyLite tables and other supported prepared result
sets with fixed-width columns.

## Binary-Size And Dependency Impact

No new dependency. Binary-size impact is limited to a few state fields and
helper routines in `libmylite`.

## Test And Verification Plan

- Extend prepared statement tests to prove:
  - fixed-width result statements can execute, drain, reset, and execute again;
  - reset before draining still discards the active result and allows reuse;
  - text/BLOB result reads still work across reset and re-execution.
- Run `mylite_embedded_statement_test`.
- Run routed storage-engine compatibility smoke.
- Run the local performance baseline and compare prepared primary-key point
  selects.
- Run `git diff --check`.

## Acceptance Criteria

- Fixed-width prepared result statements do not rebuild and rebind result
  buffers after the first successful bind.
- Fully drained result statements are not freed a second time during reset.
- Existing prepared statement and routed storage smoke tests pass.
- The benchmark still validates primary-key point-select checksums.

## Risks

- Reusing result binds across variable-length result buffers would be unsafe
  because truncation recovery can reallocate backing storage; this slice
  deliberately excludes variable text and BLOB result sets.
- Parameter bind reuse is intentionally out of scope because preserving current
  `mylite_reset()` binding-clearing semantics while skipping rebinding requires
  additional per-parameter state.

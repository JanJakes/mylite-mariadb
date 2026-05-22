# Prepared Non-Result Free Result Fast Path

## Problem

Hot prepared row-DML loops execute a statement, observe `MYLITE_DONE`, reset,
rebind scalar values, and execute again. MyLite already avoids a redundant
MariaDB statement reset in this successful non-result reset path, but
`execute_statement()` still calls `mysql_stmt_free_result()` after every
successful non-result execution.

For non-result statements there is no active result set to abandon, and
MariaDB's next `mysql_stmt_execute()` already resets client-side stored-result
state before execution. The extra post-execution call is therefore redundant
bookkeeping on the prepared insert/update/delete hot path.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/libmysqld/libmysql.c::mysql_stmt_execute()` begins by calling
  `reset_stmt_handle(stmt, RESET_STORE_RESULT | RESET_CLEAR_ERROR)`.
- `mariadb/libmysqld/libmysql.c::mysql_stmt_execute()` only prepares a result
  fetch path when `mysql->field_count` is nonzero.
- `mariadb/libmysqld/libmysql.c::mysql_stmt_free_result()` calls
  `reset_stmt_handle(stmt, RESET_LONG_DATA | RESET_STORE_RESULT |
  RESET_CLEAR_ERROR)`, which repeats client-side result cleanup and moves the
  statement back to prepare-done state.
- MyLite does not use `mysql_stmt_send_long_data()`, so the additional
  `RESET_LONG_DATA` work in the post-DML free-result call is not needed for
  current scalar or buffer-bound prepared parameters.
- Existing `packages/libmylite/tests/embedded_statement_test.c` coverage already
  re-executes prepared non-result `UPDATE` statements after `mylite_reset()` and
  checks affected-row behavior.

## Design

- Remove the unconditional `mysql_stmt_free_result()` call from the successful
  non-result prepared execution branch.
- Keep failed execute, result-producing, reset-before-drain, and finalize paths
  unchanged.
- Continue capturing warning counts immediately after successful execution.
- Keep MyLite statement state unchanged: successful non-result executions still
  set `executed=true`, `done=true`, and `result_active=false`.

## Compatibility Impact

Public API behavior remains the same. Prepared non-result statements still
return `MYLITE_DONE`, expose affected rows, retain warnings, and can be reset
and re-executed with preserved bindings.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, catalog, or companion-file lifecycle
change.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing change. This reduces prepared-statement bookkeeping above MariaDB and
MyLite storage execution.

## Binary-Size And Dependency Impact

Tiny first-party code removal. No dependency or meaningful binary-size impact.

## Tests And Verification

- Run existing embedded statement coverage, including prepared non-result
  re-execution, affected rows, warnings, and reset/finalize paths.
- Run routed storage-smoke coverage.
- Run focused prepared-update and prepared point-select benchmarks.
- Run `git diff --check` and `git clang-format --diff` on `database.cc`.

## Verification Evidence

- `git diff --check`
- `git clang-format --diff -- packages/libmylite/src/database.cc`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_statement_test mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/libmylite/mylite_embedded_statement_test`
- `ctest --test-dir build/storage-smoke-dev -R 'libmylite.embedded-statement|libmylite.embedded-storage-engine' --output-on-failure`
  - 2/2 tests passed.
- `ctest --preset storage-smoke-dev --output-on-failure`
  - 10/10 tests passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-updates 10000 1000000`
  - Prepared primary-key updates in one transaction: `2.655 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-selects 10000 1000000`
  - Prepared primary-key point selects: `9.557 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-select-reset-after-row 10000 1000000`
  - Prepared primary-key point selects reset after row: `9.569 us/op`.

## Acceptance Criteria

- Successful non-result prepared execution no longer calls
  `mysql_stmt_free_result()`.
- Prepared DML reset/re-execute behavior and affected-row reporting remain
  covered by existing tests.
- Existing statement and storage-smoke tests pass.
- Local benchmarks record whether the change is measurable.

## Risks And Unresolved Questions

- This relies on MariaDB's `mysql_stmt_execute()` resetting stored-result state
  before every execution, which is true for the selected base.
- If MyLite later adds `mysql_stmt_send_long_data()` support, long-data reset
  semantics need a separate design and tests.

# Prepared Abandoned Result Reset Fast Path

## Problem

SQLite-style prepared point-select loops often read the expected row and call
reset immediately instead of stepping to end-of-result. MyLite currently handles
that correctly, but the reset-before-drain path calls both
`mysql_stmt_free_result()` and `mysql_stmt_reset()` before the next execution.

The new `prepared-pk-select-reset-after-row` benchmark gives this workload an
explicit measurement target. The embedded MariaDB reset path suggests the second
call is redundant after `mysql_stmt_free_result()` has already abandoned and
flushed the active result.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/libmysqld/libmysql.c::mysql_stmt_free_result()` calls
  `reset_stmt_handle()` with `RESET_LONG_DATA | RESET_STORE_RESULT |
  RESET_CLEAR_ERROR`.
- `reset_stmt_handle()` flushes an active unbuffered result when the connection
  is not ready, clears the result cursor/storage, sets the read function back to
  the no-result path, and returns the connection to ready state.
- `mariadb/libmysqld/libmysql.c::mysql_stmt_execute()` starts by clearing stored
  result state and then executes the statement again; it does not require a
  preceding `COM_STMT_RESET` for a prepared statement already returned to the
  prepare-done state.
- `packages/libmylite/src/database.cc::mylite_reset()` currently calls
  `mysql_stmt_reset()` whenever an executed statement is not both done and
  result-inactive, even if it has just freed an active result.

## Design

- Track whether `mylite_reset()` abandoned an active result by calling
  `mysql_stmt_free_result()`.
- Treat either a fully drained result or a successfully abandoned active result
  as reusable without a subsequent `mysql_stmt_reset()`.
- Keep failed statement reset behavior unchanged.
- Keep non-result and fully drained result fast paths unchanged.
- Keep finalize-before-drain behavior unchanged.

## Compatibility Impact

Public API behavior remains the same: reset-before-drain discards unread rows,
the next execution starts from the beginning, and finalize still cleans up
active results.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, catalog, or companion-file lifecycle
change.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing change. This is prepared-result lifecycle bookkeeping above MariaDB
execution and MyLite storage routing.

## Binary-Size And Dependency Impact

Small first-party boolean branch. No dependency or meaningful binary-size
impact.

## Tests And Verification

- Run existing embedded statement coverage, especially reset-before-drain and
  finalize-before-drain tests.
- Run routed storage-smoke coverage.
- Run focused prepared reset-after-row and drained point-select benchmarks.
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
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-select-reset-after-row 10000 1000000`
  - Prepared primary-key point selects reset after row: `9.097 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-selects 10000 1000000`
  - Prepared primary-key point selects: `9.230 us/op`.

## Acceptance Criteria

- `mylite_reset()` does not call `mysql_stmt_reset()` after successfully freeing
  an active result.
- Reset-before-drain still discards unread rows and allows re-execution.
- Existing statement and storage-smoke tests pass.
- Local benchmarks record whether the change is measurable.

## Risks And Unresolved Questions

- This relies on embedded MariaDB `mysql_stmt_free_result()` fully abandoning
  active unbuffered results. That is true for the current base and MyLite's
  current no-cursor prepared statement configuration.

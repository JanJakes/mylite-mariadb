# Prepared Drained Result Free Fast Path

## Problem

Prepared primary-key point-select loops fetch one row, step once more to observe
`MYLITE_DONE`, reset, then re-execute. The current drained-result path calls
`mysql_stmt_free_result()` as soon as `mysql_stmt_fetch()` returns
`MYSQL_NO_DATA`, even though MyLite does not call `mysql_stmt_store_result()`
for these unbuffered embedded prepared statements.

The previous reset fast path avoids a second reset after a drained result. This
slice checks whether the first free is also redundant for the embedded
unbuffered path and removes it only after result exhaustion.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/libmysqld/libmysql.c::mysql_stmt_execute()` begins by calling
  `reset_stmt_handle()` with `RESET_STORE_RESULT | RESET_CLEAR_ERROR`, so any
  stored client-side result from a prior execution is cleared before execution.
- `mariadb/libmysqld/libmysql.c::prepare_to_fetch_result()` uses the
  unbuffered fetch path unless a cursor flag requests cursor/store behavior.
  MyLite does not set cursor attributes on prepared statements.
- `mariadb/libmysqld/libmysql.c::mysql_stmt_fetch()` switches the statement
  read function to the no-data path when the embedded read function returns
  `MYSQL_NO_DATA`.
- `mariadb/libmysqld/libmysql.c::mysql_stmt_free_result()` also routes through
  `reset_stmt_handle()` with `RESET_STORE_RESULT`, making it unnecessary after
  a fully drained unbuffered result that has no stored client-side result.

## Design

- When `mysql_stmt_fetch()` returns `MYSQL_NO_DATA`, mark the MyLite statement
  done, clear `result_active`, capture warnings, and return `MYLITE_DONE`
  without calling `mysql_stmt_free_result()`.
- Keep the early-abandoned result path unchanged: `mylite_reset()` and
  `mylite_finalize()` still call `mysql_stmt_free_result()` when `result_active`
  is true.
- Keep non-result prepared execution cleanup unchanged.
- Keep variable-length result materialization unchanged.

## Compatibility Impact

Public API behavior remains the same. Applications still see rows, done state,
warnings after drain, reset-before-drain behavior, repeated reset/re-execute,
and finalize behavior through the same `libmylite` calls.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, catalog, or companion-file lifecycle
change.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing change. This is a prepared-result lifecycle optimization above
MariaDB execution and MyLite storage routing.

## Binary-Size And Dependency Impact

Removes one embedded C API call from the drained prepared result path. No new
dependency or meaningful binary-size impact.

## Tests And Verification

- Run existing embedded statement coverage, including drained result reset and
  reset-before-drain cases.
- Run routed storage-smoke coverage.
- Run focused prepared primary-key point-select and prepared-update benchmarks.
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
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-selects 10000 1000000`
  - Prepared primary-key point selects: `9.204 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-updates 10000 1000000`
  - Prepared primary-key updates in one transaction: `2.577 us/op`.

## Acceptance Criteria

- Fully drained prepared result statements no longer call
  `mysql_stmt_free_result()` on the `MYSQL_NO_DATA` path.
- Reset-before-drain and finalize-before-drain still free active results.
- Existing statement and storage-smoke tests pass.
- Local benchmarks record whether the change is measurable.

## Risks And Unresolved Questions

- This relies on MyLite's current embedded prepared statements using unbuffered
  result fetching and not setting cursor/store attributes. If MyLite later adds
  stored prepared results, that path should either retain an explicit free or
  track stored-result ownership separately.

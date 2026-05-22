# Prepared Reset Before Drain Cache

## Problem

The local performance baseline shows ordinary prepared primary-key point
selects around `2.247 us/op`, while resetting the same statement immediately
after the first row costs roughly `5015 us/op`. Applications often reset after
the expected row instead of stepping once more to `DONE`, so this path should
not repeatedly close the prepared read scope and lose the one-row result cache
when the statement can prove that the first row was the only row.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc::mylite_reset()` currently calls
  `mysql_stmt_free_result()` for active results and closes the prepared read
  scope before reuse.
- `packages/libmylite/src/database.cc::fetch_statement_row()` stores a pending
  one-row result cache entry only after MariaDB reports `MYSQL_NO_DATA`.
- `packages/libmylite/src/database.cc::capture_one_row_result_cache_row()`
  already detects a second row and cancels one-row cache publication.
- `packages/libmylite/src/database.cc::close_prepared_read_scope_before_write()`
  closes retained prepared read scopes before writes, preventing cached rows
  from surviving storage changes.

## Design

- For fixed-width prepared statements that already qualify for the one-row
  result cache and have exactly one pending row, let `mylite_reset()` fetch one
  additional row before freeing an active result.
- If that fetch returns `DONE`, publish the one-row cache entry, mark the result
  inactive, and keep the prepared read scope for reuse.
- If that fetch returns another row, keep the existing reset-before-drain
  behavior: discard the active result and close the prepared read scope.
- Keep variable-width results on the existing path to avoid introducing new
  allocation and truncation behavior inside `mylite_reset()`.
- Preserve write invalidation through the existing prepared-read-scope owner
  check before writes.

## Compatibility Impact

The public reset semantics remain SQLite-like: reset discards any active result
and makes the statement reusable. The optimization only changes how MyLite
proves a one-row fixed-width result is exhausted before reset returns.

## Single-File And Embedded Lifecycle Impact

No file-format or durable lifecycle change. A retained read scope remains
read-only and is closed before writes through the existing guard.

## Public API And File-Format Impact

No public API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing change. The optimization applies to existing prepared read scopes
over routed MyLite storage.

## Binary-Size And Dependency Impact

No dependency impact. The library gains one bounded reset helper.

## Tests And Verification

- Add embedded statement coverage for resetting after a single primary-key row,
  then writing and verifying the cache does not survive the write.
- Keep existing multi-row reset-before-drain coverage unchanged.
- Run `libmylite.embedded-statement`.
- Run the prepared reset-after-row performance phase.
- Run `git diff --check` and clang-format diff checks.

## Verification Evidence

- `git diff --check`
- `git clang-format --diff -- packages/libmylite/src/database.cc packages/libmylite/tests/embedded_statement_test.c`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_statement_test mylite_perf_baseline`
- `ctest --preset storage-smoke-dev -R 'libmylite\.embedded-statement' --output-on-failure`
  passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-select-reset-after-row 1000 100000`
  reported reset-after-row point selects at `1.056 us/op`, down from the broad
  pre-slice sample of about `5015 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-select-components 1000 100000`
  reported bind at `0.023 us/op`, row at `0.930 us/op`, done at `0.117 us/op`,
  and reset at `0.027 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-selects 1000 100000`
  reported ordinary prepared point selects at `1.015 us/op`.

## Acceptance Criteria

- Reset-after-row still works for multi-row results by discarding unread rows.
- Reset-after-row for the fixed-width primary-key benchmark no longer closes the
  prepared read scope every iteration once one-row cache entries are learned.
- Writes after reset-before-drain invalidate retained read-scope cache state.

## Risks And Unresolved Questions

- The helper intentionally probes only one additional row. Broader uniqueness
  inference from SQL and metadata could avoid the probe, but that belongs in a
  later slice.
- Variable-width one-row reset-before-drain remains on the conservative path.

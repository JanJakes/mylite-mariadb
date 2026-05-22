# Prepared Fetch Row Bookkeeping Fast Path

## Problem

Prepared primary-key point-select loops fetch a single fixed-width value, expose
it through `mylite_column_*()`, step to `MYLITE_DONE`, reset, and repeat.
`fetch_statement_row()` currently clears every `ColumnValue` before each fetch
and then loops over the result columns twice: once for truncation checks and
once for MyLite type/length state.

For fixed-width reusable result binds, MariaDB writes the bound scalar buffers,
null flags, lengths, and error flags during `mysql_stmt_fetch()`. MyLite only
needs to hide the old row before the fetch and then publish the newly fetched
row state.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/libmysqld/libmysql.c::stmt_fetch_row()` writes every bound result
  column's error flag and null flag, and invokes each bound fetch function for
  non-null values.
- `packages/libmylite/src/database.cc::initialize_statement_metadata()` marks
  fixed-width result sets as `reusable_result_binds`; variable text/BLOB result
  sets keep conservative per-execution result binding.
- `packages/libmylite/src/database.cc::clear_current_row()` resets per-column
  metadata but does not clear scalar payload fields, so current correctness
  already depends on `has_current_row` and MariaDB fetch overwrites rather than
  zeroed scalar payloads.
- `packages/libmylite/src/database.cc::fetch_statement_row()` separately loops
  over result columns for numeric truncation checks and result-state publication.

## Design

- Add a private helper for clearing current-row visibility before another
  execution or fetch.
- For reusable fixed-width result binds, clear only `has_current_row`.
- For variable result binds, keep the full per-column clear because backing
  byte buffers and truncation/materialization state need conservative handling.
- Merge the two post-fetch loops into one loop that:
  - maps the MariaDB column type once;
  - rejects non-variable truncated columns;
  - publishes the MyLite value type;
  - updates variable-column byte completeness and text NUL termination.

## Compatibility Impact

Public API behavior remains the same. Stale row values remain inaccessible when
`has_current_row` is false, and fetched row values continue to come from the
MariaDB result bind buffers.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, catalog, or companion-file lifecycle
change.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing change. This reduces `libmylite` prepared-result bookkeeping after
MariaDB and the storage engine have produced a row.

## Binary-Size And Dependency Impact

Small first-party helper and one fewer loop in `fetch_statement_row()`. No new
dependency or meaningful binary-size impact.

## Tests And Verification

- Run existing embedded statement coverage, including scalar, text/BLOB,
  segment-read, reset-before-drain, and repeated result reset cases.
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
  - Prepared primary-key point selects: `8.998 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-updates 10000 1000000`
  - Prepared primary-key updates in one transaction: `2.583 us/op`.

## Acceptance Criteria

- Fixed-width reusable result statements avoid full per-column current-row
  clearing before each fetch.
- `fetch_statement_row()` publishes row state with one post-fetch column loop.
- Variable text/BLOB result handling remains conservative.
- Existing statement and storage-smoke tests pass.
- Local benchmarks record whether the change is measurable.

## Risks And Unresolved Questions

- The fixed-width fast path relies on MariaDB filling result bind null/error
  fields on every successful fetch. That matches the embedded fetch code and is
  already required for the existing reusable result bind path.

# Prepared Scalar Result Benchmark

## Problem

Prepared primary-key point-select timings currently include MariaDB prepared
execution, MyLite C API state handling, result binding/fetch, handler planning,
and MyLite storage lookup. That makes it hard to tell how much of the remaining
cost can be improved by storage/pager work alone.

MyLite needs a lower-bound benchmark for result-producing prepared statements
that do not touch routed storage.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/libmysqld/libmysql.c::mysql_stmt_execute()` is still the execution
  authority for prepared statements, including scalar `SELECT` statements.
- `packages/libmylite/src/database.cc::execute_statement()` and
  `execute_simple_result_statement()` keep result-producing prepared statements
  on MariaDB execution and MyLite result binding/fetch paths.
- `tools/mylite_perf_baseline.c` already measures routed prepared primary-key
  selects and reset-after-row prepared selects, but it has no table-free scalar
  result phase.

## Design

Add a focused `prepared-scalar-selects` phase to the local performance baseline:

- open the same MyLite database configuration as the other phases;
- prepare `SELECT ? + 1`;
- for each iteration, bind one integer parameter, step to the single result
  row, read and validate the integer value, step to `MYLITE_DONE`, and reset;
- print a checksum so the loop cannot be optimized away;
- expose the phase in `--phase` parsing, usage text, metric thresholds, and
  README examples.

The phase intentionally does not read or write MyLite storage tables. Setup
time remains reported separately and is not part of the metric.

## Compatibility Impact

No SQL or public API behavior changes. This is measurement-only and uses
existing prepared statement APIs.

## Single-File And Embedded Lifecycle Impact

No file-format, journal, recovery, lock, catalog, or companion-file lifecycle
change.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

The benchmark phase deliberately avoids routed storage. It exists to compare
against routed point-select phases.

## Binary-Size And Dependency Impact

No new dependency. Binary impact is limited to benchmark code and documentation.

## Tests And Verification

- Build the storage-smoke performance target.
- Run the new focused scalar phase.
- Run at least one existing prepared point-select phase to confirm parser
  changes did not break existing phase selection.
- Run `git diff --check` and formatting checks for the C benchmark file.

## Acceptance Criteria

- `tools/mylite-perf-baseline --phase=prepared-scalar-selects` is accepted.
- The phase reports `prepared scalar selects` with a checksum.
- Threshold parsing accepts `prepared-scalar-selects`.
- Existing point-select and update phase names remain accepted.

## Verification Evidence

- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-scalar-selects --max-us=prepared-scalar-selects:50 10000 1000000`
  - Prepared scalar selects: `0.747 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-selects 10000 100000`
  - Prepared primary-key point selects: `9.787 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --help`

## Risks And Unresolved Questions

- The scalar phase is a lower bound, not a storage performance benchmark. It
  should guide storage work but cannot prove SQLite-like point-read behavior.
- The result remains machine-local and should not be treated as a portable
  performance guarantee.

# Query Log Trim

## Problem Statement

The default embedded archive still builds MariaDB's general and slow query log
runtime. These logs are daemon diagnostics that write query text to server log
files or `mysql.*` log tables. MyLite already exposes statement errors,
warnings, and result metadata through `libmylite`; inherited query logging is
not durable application data, native storage behavior, or part of the
directory-owned embedded API contract.

## Source Findings

- Base ref: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/log.cc` owns `LOGGER` query-log dispatch,
  `Log_to_csv_event_handler`, `Log_to_file_event_handler`, and
  `MYSQL_QUERY_LOG::write()` formatting for general and slow logs.
- `mariadb/sql/sys_vars.cc` registers query-log controls such as
  `general_log`, `slow_query_log`, `log_slow_query`, `log_output`,
  `general_log_file`, `slow_query_log_file`, `log_slow_query_file`,
  `sql_log_off`, and slow-log tuning variables.
- `mariadb/sql/sql_parse.cc` and prepared-statement paths still call
  `general_log_write()`, `general_log_print()`, and `slow_log_print()`.
  The safe cut is therefore inert query-log handlers, not deleting the shared
  logging object that also provides error-log entry points.

## Design

Add `MYLITE_WITH_QUERY_LOGS`, defaulting to `ON` for upstream-style builds and
forced `OFF` in `cmake/mariadb-embedded-baseline.cmake`.

When disabled, the embedded build keeps the `LOGGER` object and error-log
printing, but compiles general and slow query log dispatch, table handlers,
file handlers, and `MYSQL_QUERY_LOG` write/reopen methods to no-op embedded
stubs. Startup passes `--log-output=NONE` so the runtime advertises the
disabled query-log output mode.

The public MyLite SQL policy rejects:

- assignments to general-log variables,
- assignments to slow-query-log variables,
- assignments to `log_output` and `sql_log_off`,
- `FLUSH LOGS`, `FLUSH GENERAL LOGS`, and `FLUSH SLOW LOGS`, including
  `LOCAL` and `NO_WRITE_TO_BINLOG` variants.

Ordinary SQL execution, errors, warnings, transactions, native storage, and
`EXPLAIN` remain unchanged.

## Compatibility Impact

General and slow query logs become explicitly unsupported in the default
embedded profile. This removes server query-log files and log-table output, not
SQL syntax, query execution, DDL, DML, JSON SQL functions, GEOMETRY/GIS,
native storage engines, or the public C API.

Query-log configuration variables remain readable for compatibility evidence,
but attempts to enable or configure query logging fail with MyLite's stable
unsupported-surface diagnostic.

## Database-Directory And Lifecycle Impact

The slice reduces inherited daemon log-file behavior. It does not add durable
files, temporary files, locks, metadata, or runtime directories. The default
runtime should not create general or slow query log files inside or outside the
MyLite database directory.

## Public API Impact

None. `libmylite` headers and symbols are unchanged. Direct execution and
prepared statements return stable MyLite unsupported-surface diagnostics for
query-log SQL.

## Native Storage Impact

None. Native engine routing, table files, transaction behavior, and recovery
behavior are unchanged.

## Binary-Size Impact

Measured with `tools/mariadb-embedded-build measure`, the stripped embedded
archive is 27,095,640 bytes / 25.84 MiB with 705 members. This is 21,168
bytes smaller than the previous optimizer-trace baseline and does not change
archive membership.

## Test And Verification Plan

- Run `tools/mariadb-embedded-build all`.
- Confirm `MYLITE_WITH_QUERY_LOGS=OFF` appears in the embedded cache summary.
- Run `cmake --build --preset embedded-dev`.
- Run `ctest --preset embedded-dev -L compat.server-surface --output-on-failure`.
- Run `ctest --preset embedded-dev --output-on-failure`.
- Run `cmake --build --preset dev`.
- Run `ctest --preset dev --output-on-failure`.
- Run `cmake --build --preset embedded-dev --target format-check`.
- Run `cmake --build --preset dev --target tidy`.
- Run `cmake --build --preset embedded-dev --target tidy`.
- Run `git diff --check`.
- Run `tools/mariadb-embedded-build measure`.

## Acceptance Criteria

- The embedded baseline configures `MYLITE_WITH_QUERY_LOGS=OFF`.
- General and slow query-log handlers are inert in the embedded archive.
- Query-log configuration SQL is rejected for direct execution and prepared
  statements.
- `@@general_log`, `@@slow_query_log`, and `@@log_output` show the disabled
  embedded state.
- Architecture, compatibility, API, roadmap, and size-profile docs describe the
  unsupported diagnostic surface and measured size impact.

## Risks And Unresolved Questions

- `LOGGER` also owns error-log printing, so this slice must not remove the
  shared logging object or error-log handler.
- MariaDB still registers query-log system variables. MyLite policy rejects
  writes before users can enable a misleading no-op diagnostic surface.

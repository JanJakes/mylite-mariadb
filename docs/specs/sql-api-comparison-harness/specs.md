# SQL API Comparison Harness

## Problem

The SQL execution API has focused unit and embedded tests for direct
execution, prepared statements, metadata, warnings, and large values. The
roadmap still needs broader comparison evidence that MyLite's public API
continues to reflect the MariaDB embedded baseline for representative SQL API
behavior.

This slice adds an initial MariaDB comparison test without importing the full
MTR suite.

## Scope

- Compare `mylite_exec()` row values and column names against direct
  `mysql_query()` / `mysql_store_result()` behavior for a representative
  result set.
- Compare prepared statement parameter binding, row values, column names, and
  native metadata against direct `MYSQL_STMT` behavior.
- Compare warning count and first structured warning row against direct
  `SHOW WARNINGS` behavior.
- Expose the comparison as a first-party compatibility harness group.

## Non-Goals

- Do not import or normalize MariaDB MTR.
- Do not run against an external daemon or socket.
- Do not compare MyLite-routed durable storage behavior in this slice.
- Do not add application-schema comparison coverage.
- Do not change public API behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/include/mysql.h` declares `mysql_server_init()` and
  `mysql_server_end()` for embedded runtime setup and teardown.
- `mariadb/include/mysql.h` declares the direct query APIs used by the current
  MyLite `mylite_exec()` path: `mysql_query()`, `mysql_store_result()`,
  `mysql_fetch_fields()`, `mysql_fetch_row()`, `mysql_fetch_lengths()`, and
  `mysql_warning_count()`.
- `mariadb/include/mysql.h` declares the prepared statement APIs used by the
  current MyLite statement path: `mysql_stmt_prepare()`,
  `mysql_stmt_bind_param()`, `mysql_stmt_execute()`,
  `mysql_stmt_result_metadata()`, `mysql_stmt_bind_result()`,
  `mysql_stmt_fetch()`, and `mysql_stmt_fetch_column()`.
- `mariadb/libmysqld/libmysql.c` implements `mysql_query()` through
  `mysql_real_query()`, returns `mysql->warning_count` from
  `mysql_warning_count()`, and keeps `mysql_stmt_fetch_column()` tied to a
  current fetched row with zero-based column indexes and byte offsets.

## Design

Add an embedded-only comparison test binary that runs in two separate phases:

1. Start a raw MariaDB embedded runtime with the same local-file defaults that
   MyLite uses for non-storage-engine embedded tests: no defaults, private
   datadir/tmp/plugin directories, skip grants, skip binlog, skip networking,
   MyISAM default storage, InnoDB off, and the configured message/charset
   directories.
2. Execute representative direct, prepared, metadata, and warning probes
   through the MariaDB C API, copy the observed values, then close the
   connection and call `mysql_server_end()`.
3. Open a MyLite database with its normal embedded runtime and run the same SQL
   through `libmylite`.
4. Compare normalized values that the public MyLite API is supposed to expose:
   column names, row string values for direct execution, prepared scalar
   values, native MariaDB type numbers, warning count, warning level, warning
   code, and warning message.

The test intentionally avoids durable table DDL. That keeps this slice focused
on SQL API behavior and avoids comparing MariaDB sidecar behavior against the
MyLite storage-engine roadmap.

## Compatibility Impact

This moves the SQL execution API from self-contained smoke coverage toward
baseline-backed compatibility coverage. It does not claim broad MTR coverage;
the comparison group is an initial guardrail for representative API surfaces.

## Single-File And Storage Impact

No file-format or storage behavior changes. The raw MariaDB baseline uses only
temporary runtime directories. The MyLite half opens a normal `.mylite` file
but does not exercise routed durable DDL.

## Embedded Lifecycle Impact

The comparison test starts and ends the raw MariaDB embedded runtime before
opening the MyLite handle, so both phases use the process-global MariaDB
runtime sequentially rather than concurrently.

## Build, Size, License, And Dependencies

No new dependency is added. The comparison binary links against the existing
MariaDB embedded archive and `libmylite`; linked test binary size should be
measured but does not change the shipped library surface.

## Test Plan

1. Add `mylite_embedded_comparison_test`.
2. Label it with `compat-sql-comparison`, `compat-direct-sql`,
   `compat-prepared-statement`, `compat-column-metadata`, and
   `compat-warning`.
3. Add a `sql-comparison` compatibility harness group.
4. Run `dev`, `embedded-dev`, `storage-smoke-dev`, the new comparison harness
   group, format, tidy, diff checks, shell checks, and size report.

## Acceptance Criteria

- The comparison test proves direct row values and column names match the
  direct MariaDB embedded baseline for the selected probe.
- The comparison test proves prepared binding output and native metadata match
  the direct MariaDB embedded baseline for the selected probe.
- The comparison test proves warning count and first warning row match the
  direct MariaDB embedded baseline for the selected probe.
- Compatibility docs and harness docs describe the comparison group without
  overstating MTR coverage.

## Risks And Open Questions

- The initial comparison set is intentionally representative, not exhaustive.
- Raw MariaDB runtime initialization is process-global, so tests must keep the
  raw baseline and MyLite phases sequential.
- Future comparison slices should decide whether to add curated MTR subsets,
  application-schema comparisons, or external daemon comparisons.

# Failed Warning Enumeration

## Problem

MyLite already captures structured `SHOW WARNINGS` rows after successful
direct and prepared execution. Failed direct execution and failed prepared
prepare/execute paths currently preserve diagnostics but drop the structured
warning/error rows that MariaDB exposes through the same warning channel.

Applications often inspect warning rows after failed SQL to distinguish syntax
errors, missing objects, truncation errors, and server diagnostics. MyLite
should retain those rows where the embedded lifecycle is safe.

## Scope

- Capture warning rows after failed `mylite_exec()` calls.
- Capture warning rows after failed `mylite_prepare()` calls.
- Capture warning rows after failed prepared `mylite_step()` execution before
  a result set is active.
- Preserve the original public MyLite error code, MariaDB errno, SQLSTATE, and
  message when warning capture succeeds.
- Extend warning compatibility tests and docs.

## Non-Goals

- Do not capture warnings for fetch-time failures while a prepared result set
  may still be active.
- Do not change the existing warning enumeration API.
- Do not expose a separate error-list API beyond MariaDB warning rows.
- Do not change server-surface policy failures, which are MyLite rejections
  rather than MariaDB diagnostics.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/include/mysql.h` declares `mysql_warning_count(MYSQL *)` and the
  direct execution functions used by `mylite_exec()`.
- `mariadb/libmysqld/libmysql.c` returns `mysql->warning_count` from
  `mysql_warning_count()`.
- `SHOW WARNINGS` is already used by MyLite to materialize structured rows
  after successful execution.
- Prepared statement prepare and execute failures occur before MyLite binds or
  drains an active result set, so issuing `SHOW WARNINGS` on the connection is
  safe in those paths.

## Design

Reuse the existing `capture_warnings()` path after storing the original
diagnostic state on the database handle. The helper already reads
`SHOW WARNINGS` into MyLite-owned `StoredWarning` rows and does not clear the
database error state on success.

For direct `mysql_query()` failure:

1. Read `mysql_warning_count()` as a hint.
2. Store MariaDB diagnostics with `set_mariadb_error()`.
3. Force a `SHOW WARNINGS` read so error rows are retained even when the
   connection warning count is zero.
4. Return the original error unless warning capture itself fails.

For `mysql_stmt_prepare()` and `mysql_stmt_execute()` failure:

1. Read the connection warning count as a hint.
2. Store statement diagnostics with `set_mariadb_statement_error()`.
3. Force a `SHOW WARNINGS` read through the owning database connection.
4. Preserve the original statement failure unless warning capture itself fails.

Fetch-time failures remain explicit future work because `SHOW WARNINGS` during
an active result lifecycle needs separate result cleanup rules.

## Compatibility Impact

Warning enumeration moves closer to MariaDB behavior for failed SQL. The
compatibility matrix should no longer describe failed-statement warning
enumeration as fully planned; it remains partial only for fetch-time failures.

## Single-File, Storage, And Embedded Lifecycle Impact

No file-format, storage, or sidecar behavior changes. The additional
`SHOW WARNINGS` queries run on the existing embedded connection before a new
statement result is active.

## Build, Size, License, And Dependencies

No new dependency or public symbol is added. Size impact should be negligible
and measured by the normal size report.

## Test Plan

1. Extend embedded warning tests for failed direct execution.
2. Extend embedded warning tests for failed prepare.
3. Extend embedded warning tests for failed prepared execute before a result
   set is active.
4. Run `dev`, `embedded-dev`, `storage-smoke-dev`, the warning compatibility
   group, format, tidy, diff checks, shell checks, and size report.

## Acceptance Criteria

- Failed direct SQL preserves MariaDB error diagnostics and stores at least the
  first structured error row.
- Failed prepared SQL preserves MariaDB statement diagnostics and stores at
  least the first structured error row for prepare and execute failures.
- A later successful warning-free statement clears prior failure warning rows.
- Docs and compatibility claims match the implemented failure coverage.

## Risks And Open Questions

- Fetch-time failure warning capture is deliberately left for a separate slice.
- `SHOW WARNINGS` itself can fail; in that case MyLite should report the
  warning-capture failure rather than silently claiming rows were retained.

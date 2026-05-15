# Prepared Statement API

## Goal

Add the first reusable, typed, binary-safe SQL execution surface to
`libmylite`: prepared statements with scalar parameter binding, row stepping,
reset/finalize ownership, and typed column access.

This completes the next public API step after `mylite_exec()`. The new API
should remain file-owned and MyLite-shaped while using MariaDB's prepared
statement machinery for SQL parsing, execution, diagnostics, affected rows, and
insert ids.

## Non-Goals

- Do not expose raw `MYSQL_STMT *` handles.
- Do not implement multi-statement prepare or multi-result stepping.
- Do not add rich metadata APIs for schema/table/original-name, precision,
  scale, collation, or MariaDB-native field types.
- Do not implement warning enumeration.
- Do not add transaction, savepoint, rollback, or concurrency semantics beyond
  the current storage and embedded runtime behavior.
- Do not broaden server-oriented SQL support; prepared statements must use the
  same policy gate as `mylite_exec()`.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/include/mysql.h` declares `MYSQL_STMT`, `MYSQL_BIND`,
  `mysql_stmt_init()`, `mysql_stmt_prepare()`, `mysql_stmt_bind_param()`,
  `mysql_stmt_execute()`, `mysql_stmt_bind_result()`,
  `mysql_stmt_fetch()`, `mysql_stmt_fetch_column()`,
  `mysql_stmt_free_result()`, `mysql_stmt_reset()`,
  `mysql_stmt_close()`, `mysql_stmt_param_count()`,
  `mysql_stmt_result_metadata()`, `mysql_stmt_field_count()`,
  `mysql_stmt_errno()`, `mysql_stmt_sqlstate()`,
  `mysql_stmt_error()`, `mysql_stmt_affected_rows()`, and
  `mysql_stmt_insert_id()`.
- `mariadb/include/mysql.h` documents the `MYSQL_BIND` contract:
  variable-length inputs use `buffer_length` and/or `length`, `is_null`
  marks nullable inputs and outputs, `is_unsigned` marks unsigned numeric
  values, and output `length` receives column byte counts.
- `mariadb/include/mysql.h` defines `MYSQL_NO_DATA` and
  `MYSQL_DATA_TRUNCATED` as prepared-statement fetch status values.
- `mariadb/include/mysql_com.h` defines the MariaDB field type enum. MyLite
  should initially map integer types to signed/unsigned 64-bit values, float
  and double to double, BLOB-family types to BLOB, string/date/time/decimal
  types to TEXT, and `MYSQL_TYPE_NULL` or row NULLs to `MYLITE_TYPE_NULL`.
- `mariadb/libmariadb/libmariadb/mariadb_stmt.c` keeps statement errors in the
  statement handle and exposes them through `mysql_stmt_errno()`,
  `mysql_stmt_sqlstate()`, and `mysql_stmt_error()`. It copies affected rows
  and insert id from the underlying connection after execution.

## Design

Expose the documented `mylite_stmt` API in `packages/libmylite/include`:

- `mylite_prepare()`,
- `mylite_step()`,
- `mylite_reset()`,
- `mylite_finalize()`,
- parameter count and binding functions,
- column count, names, value types, scalar getters, byte counts, text, and blob
  access.

`mylite_prepare()` validates public arguments, creates a `MYSQL_STMT` from the
database-owned embedded connection, runs the same server-surface policy gate as
`mylite_exec()`, prepares exactly one SQL string, initializes parameter slots to
SQL NULL, records result metadata, and increments the database statement count.
`tail`, when requested, points to the first byte after the provided SQL string;
multi-statement tail parsing is not claimed in this slice.

`mylite_step()` binds current parameter values and executes the statement on
the first call after prepare/reset. For result-producing statements it binds
output buffers, fetches one row per call, returns `MYLITE_ROW` while rows are
available, and returns `MYLITE_DONE` after `MYSQL_NO_DATA`. For non-result
statements it returns `MYLITE_DONE` after execution. A second call after done
continues to return `MYLITE_DONE` until `mylite_reset()`.

Text and BLOB output must be binary safe: `mylite_column_bytes()` reports the
exact byte count, and `mylite_column_blob()` may contain embedded NUL bytes.
`mylite_column_text()` returns a NUL-terminated view for text values while the
byte count remains authoritative.

Binding lifetime follows the API contract:

- `MYLITE_TRANSIENT` copies text/blob input before the bind call returns.
- `MYLITE_STATIC` borrows bytes until reset, rebind, clear, or finalize.
- Custom destructors run when MyLite no longer needs the input pointer.

`mylite_close()` must return `MYLITE_BUSY` when statements remain active so
database ownership remains explicit.

## Compatibility Impact

Prepared statements move the public SQL API from text-only convenience toward
application-usable drop-in behavior. The first compatibility target is local
embedded execution with:

- 1-based parameter binding,
- reusable statements after reset,
- SQL NULL, signed integer, unsigned integer, double, text, and BLOB values,
- binary-safe input and output byte counts,
- MariaDB diagnostics on prepare/execute/fetch failures,
- affected-row and insert-id updates for successful non-result execution.

Broader compatibility continues through follow-up slices: warning enumeration
and column metadata have separate specs, while array/bulk binding, streaming
large values, multi-results, and MariaDB/MySQL comparison suites remain
planned.

`docs/COMPATIBILITY.md` should move prepared statements and binary-safe values
from planned to partial when tests land.

## Single-File And Storage Impact

No file-format change is required. Prepared statements execute through the same
embedded connection and MyLite storage engine paths as `mylite_exec()`. Storage
durability and sidecar behavior remain governed by the existing routed DDL/DML
and storage-engine tests.

## Embedded Lifecycle And API

The database owns each prepared statement. Statements own their MariaDB
`MYSQL_STMT` handle, parameter storage, result buffers, metadata names, and
current-row values. Column value pointers remain valid until the next
`mylite_step()`, `mylite_reset()`, or `mylite_finalize()` on the statement.

`mylite_finalize(NULL)` returns `MYLITE_OK`; other statement APIs return
`MYLITE_MISUSE` for `NULL` statement handles or invalid indexes.

## Build, Size, And Dependencies

No new dependency is added. The implementation links MariaDB prepared statement
entry points from the existing embedded archive, so size should be measured but
not treated as a profile regression unless it pulls in unexpected server
surface.

## Test Plan

1. Extend public API tests for NULL handles, invalid bind indexes, invalid
   column indexes, `mylite_finalize(NULL)`, and close-with-active-statement
   returning `MYLITE_BUSY`.
2. Add embedded prepared-statement tests for:
   - scalar `SELECT ?` values;
   - binary-safe text and BLOB round trips with embedded NUL bytes;
   - reusable statements after reset;
   - affected rows and insert ids after parameterized insert;
   - syntax/prepare diagnostics;
   - server-oriented SQL rejection through prepare.
3. Add a compatibility harness group for prepared statements.
4. Run `dev`, `embedded-dev`, and `storage-smoke-dev` tests, the prepared
   compatibility group, format, tidy, and diff checks.
5. Run the size report and record only if measurements materially change.

## Acceptance Criteria

- The public header exposes the documented prepared statement API.
- Prepared statement ownership prevents closing a database while active
  statements exist.
- Parameter binding is 1-based and covers NULL, signed/unsigned integers,
  double, text, and BLOB values.
- Column access is 0-based, typed, and binary safe for text/BLOB values.
- Embedded tests cover prepare, bind, step, reset, finalize, diagnostics,
  binary values, affected rows, and insert ids.
- Compatibility docs and the harness describe the new partial coverage.

## Risks And Open Questions

- MariaDB's prepared statement API is client-shaped. It is acceptable for this
  first slice because it remains private behind `libmylite`; future size work
  may replace it with a narrower MyLite-owned adapter if the linked footprint is
  too high.
- Binding `MYLITE_STATIC` values across `mylite_reset()` would make caller
  lifetime hard to reason about. This slice treats reset as the end of the
  borrow and requires rebinding before the next execution when borrowed values
  were used.
- Large result values may require follow-up streaming APIs. This slice fetches
  the current row into statement-owned memory so column pointers are stable
  until the next step/reset/finalize.

# Direct SQL Execution

## Goal

Add the first SQL execution surface to `libmylite`: a `mylite_exec()` convenience
API that runs one SQL string through the embedded MariaDB connection owned by a
`mylite_db` handle and returns textual result rows through a callback.

This slice exists to prove the embedded runtime can execute controlled SQL
inside the process. It is also the prerequisite for later native-storage and
directory-lifecycle smoke tests.

## Non-Goals

- Do not implement prepared statements, bindings, typed columns, or binary-safe
  values.
- Do not implement persistent table DDL or DML.
- Do not claim durable SQL state inside the MyLite database directory.
- Do not configure native MariaDB engines as durable application storage.
- Do not support multi-statement execution or streaming result APIs.

## Source Findings

- MariaDB base: `mariadb-11.8.6` / `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/include/mysql.h:480-499` declares `mysql_query()`,
  `mysql_real_query()`, and `mysql_store_result()`.
- `mariadb/include/mysql.h:413-429` declares result metadata helpers including
  `mysql_num_fields()`, `mysql_fetch_fields()`, and `mysql_field_count()`.
- `mariadb/include/mysql.h:588-602` declares `mysql_free_result()`,
  `mysql_fetch_row()`, and `mysql_fetch_lengths()`.
- `mariadb/libmysqld/lib_sql.cc:296-334` copies embedded query metadata,
  affected rows, insert id, warnings, and errors from the embedded THD into the
  `MYSQL` connection.
- `mariadb/libmysqld/lib_sql.cc:1293-1313` records OK response affected rows,
  insert id, and info text in the embedded result data structure.

## Design

Expose `mylite_exec()` with the documented SQLite-like callback shape:

```c
typedef int (*mylite_exec_callback)(
    void *ctx,
    int column_count,
    char **values,
    char **column_names);
```

The implementation should:

- validate `db` and `sql` before calling MariaDB;
- clear `errmsg` to `NULL` before execution when provided;
- call `mysql_query()` on the handle's embedded `MYSQL` connection;
- call `mysql_store_result()` for result-producing statements;
- call the callback once per row with MariaDB text values and column names;
- preserve SQL `NULL` values as `NULL` callback entries;
- copy MariaDB errno, SQLSTATE, and error message into the handle diagnostics on
  failure;
- return an allocated error string through `errmsg` when requested, released by
  `mylite_free()`;
- update `mylite_changes()` from MariaDB after successful non-result
  statements, keep it zero for result-producing statements, and update
  `mylite_last_insert_id()` after successful statements.

The callback value pointers remain owned by MariaDB and are valid only for the
duration of the callback. Callers that need stable values must copy them.

## Compatibility Impact

This slice adds partial public API compatibility only. It proves local in-process
execution for simple statements such as `SELECT 1`, but it does not claim table
DDL, DML durability, prepared statements, warning enumeration, or binary-safe
value access.

`mylite_exec()` follows SQLite's callback ergonomics, while SQL behavior and
diagnostics come from MariaDB. The result callback returns text because this is a
convenience API; typed values belong to prepared statements.

## File Lifecycle

This slice originally avoided durable application tables because storage still
used bootstrap runtime scaffolding. The native-storage baseline now runs durable
database paths inside the MyLite database directory, so later direct-execution
tests may create controlled tables when the relevant storage and lifecycle
behavior is covered by the slice spec.

## Test Plan

1. Add C API validation coverage for `mylite_exec(NULL, ...)`,
   `mylite_exec(db, NULL, ...)`, and `mylite_free(NULL)`.
2. Add embedded SQL smoke coverage for `SELECT 1`.
3. Verify column names and `NULL` callback values.
4. Verify a non-zero callback return aborts execution, populates diagnostics,
   and copies an optional `errmsg`.
5. Verify MariaDB syntax errors populate handle diagnostics and optional
   `errmsg` memory.
6. Verify runtime cleanup still removes the temporary MariaDB directory on close.

## Acceptance Criteria

- `mylite_exec()` is public and implemented for embedded-backend builds.
- `mylite_free()`, `mylite_changes()`, and `mylite_last_insert_id()` are public.
- Embedded tests prove successful row callbacks and syntax-error diagnostics.
- `docs/COMPATIBILITY.md` reflects direct SQL execution as partial, not complete.
- No persistent SQL table state is created or claimed.

## Risks And Open Questions

- Multi-statement behavior is deliberately unsupported until result-set draining
  and compatibility expectations are specified.
- Text callbacks are not sufficient for binary-safe application behavior; that
  remains part of prepared statements.
- MariaDB warning enumeration is still planned and must not be inferred from
  successful `mylite_exec()` execution.

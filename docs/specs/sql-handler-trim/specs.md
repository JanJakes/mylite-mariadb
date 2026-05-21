# SQL Handler Trim

## Problem

MyLite's embedded profile keeps MariaDB's native storage-engine handler
abstraction, but still ships the SQL `HANDLER ...` command runtime in
`sql/sql_handler.cc`. That command exposes a low-level server table-cursor
interface with long-lived per-session open handlers.

The top-level SQL command is not part of MyLite's core embedded API or normal
MySQL/MariaDB application SQL path. It also adds extra cleanup hooks in DDL,
temporary-table, lock, and THD teardown paths.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_handler.cc` implements top-level `HANDLER ... OPEN`,
  `HANDLER ... READ`, and `HANDLER ... CLOSE` command execution.
- `mariadb/sql/sql_handler.h` declares command entry points plus cleanup hooks
  used by generic server paths such as `sql_base.cc`, `sql_class.cc`,
  `sql_admin.cc`, `sql_db.cc`, `sql_table.cc`, `sql_trigger.cc`, and
  `temporary_tables.cc`.
- `mariadb/sql/sql_parse.cc` dispatches `SQLCOM_HA_OPEN`,
  `SQLCOM_HA_READ`, and `SQLCOM_HA_CLOSE` to the `mysql_ha_*` command entry
  points.
- `mariadb/sql/sql_prepare.cc` uses `mysql_ha_read_prepare()` for prepared
  `HANDLER ... READ` metadata.
- Historical size research measured the `HANDLER` command trim as a safe
  server-surface cut while keeping the generic storage-engine handler
  abstraction.

## Design

Add `MYLITE_WITH_SQL_HANDLER`. Normal MariaDB builds keep the upstream
`sql_handler.cc`. The MyLite embedded baseline sets the option to `OFF` and
replaces it with `mylite_sql_handler_disabled.cc`.

The disabled embedded source:

- returns an unsupported error from command entry points;
- keeps generic cleanup hooks as no-ops because no SQL handlers can be opened;
- keeps the `SQL_HANDLER` destructor/reset link contract for retained
  declarations;
- does not touch `handler`, `handlerton`, storage engines, multi-range reads,
  or normal table execution.

MyLite policy rejects direct and prepared top-level SQL `HANDLER` statements
before MariaDB dispatch.

## Compatibility Impact

The low-level SQL `HANDLER ...` command family becomes an explicit unsupported
server surface. Ordinary `SELECT`, `INSERT`, `UPDATE`, `DELETE`, prepared
statements, transactions, native storage engines, JSON, GEOMETRY, and the
embedded C API should not change.

Stored-program `DECLARE ... HANDLER` syntax is separate error-handling syntax
and is not targeted by this slice.

## Database Directory And Native Storage Impact

No durable paths, temporary paths, locks, metadata files, or native storage
engine files change. The slice only removes the top-level SQL cursor command
runtime from the embedded archive.

## Binary Size Impact

Measured impact is 12,112 bytes from the stripped archive with no archive
member count change: `sql_handler.cc.o` leaves `libmariadbd.a`, and a smaller
disabled embedded source takes its place.

## Test And Verification Plan

- Run `tools/mariadb-embedded-build all`.
- Confirm `MYLITE_WITH_SQL_HANDLER=OFF` appears in the embedded CMake cache.
- Confirm `sql_handler.cc.o` is absent from `libmariadbd.a` and
  `mylite_sql_handler_disabled.cc.o` is present.
- Confirm direct and prepared top-level SQL `HANDLER` statements fail through
  server-surface policy coverage.
- Run the embedded and default CMake builds and tests.
- Run format, tidy, `git diff --check`, and archive measurement.

## Acceptance Criteria

- Ordinary SQL execution, prepared statements, native storage, transactions,
  recovery, and compatibility harness tests pass.
- SQL `HANDLER ...` has a stable unsupported diagnostic before direct execution
  and prepared-statement dispatch.
- The measured embedded archive size and member count are updated in the build
  docs.
- Documentation clearly distinguishes SQL `HANDLER` commands from the retained
  storage-engine handler abstraction.

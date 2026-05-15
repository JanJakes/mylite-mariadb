# Prepared Constraint Diagnostics

## Problem

MyLite exposes MariaDB diagnostics through `libmylite` prepared statements, but
the CHECK and generated-column compatibility docs still list prepared
diagnostics as planned. Direct execution and generic prepared duplicate-key
failure paths already have coverage. The missing evidence is representative
prepared execution failures against MyLite-routed constraint surfaces.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/include/mysql.h` exposes `mysql_stmt_errno()`,
  `mysql_stmt_sqlstate()`, `mysql_stmt_error()`, and
  `mysql_warning_count()` for prepared statement diagnostics.
- `mariadb/libmariadb/libmariadb/mariadb_stmt.c` keeps execution errors on
  `MYSQL_STMT` and exposes them through the statement diagnostic APIs.
- `packages/libmylite/src/database.cc:execute_statement()` maps failed
  `mysql_stmt_execute()` calls through `set_mariadb_statement_error()` and
  then captures warning rows with `capture_warnings(..., true)`.
- `mariadb/sql/table.cc:TABLE::verify_constraints()` evaluates CHECK
  constraints before handler writes unless `check_constraint_checks=OFF` is
  active.
- Generated-column unique-key failures use the same MariaDB-generated key tuple
  and duplicate-key reporting path as ordinary supported unique keys.

## Design

Do not add new public API. Prove that the existing prepared execution path
retains useful diagnostics for representative MyLite-routed constraint
failures:

- a prepared insert that violates a CHECK constraint, and
- a prepared insert that violates a generated-column unique index.

The storage-engine smoke helper should assert:

- `mylite_step()` returns `MYLITE_ERROR`,
- the database result code is `MYLITE_ERROR`,
- MariaDB errno is populated,
- SQLSTATE is not the success state,
- `mylite_errmsg()` contains the expected failure text, and
- the retained first warning row is an error row with the same MariaDB errno
  and expected text.

## Supported Scope

- Prepared execution diagnostics for representative CHECK constraint failures
  on routed tables.
- Prepared execution diagnostics for representative generated-column
  duplicate-key failures on routed tables.
- Warning-row retention immediately after prepared execution failure.

## Non-Goals

- New diagnostic result-code classification such as mapping CHECK failures to
  `MYLITE_CONSTRAINT`.
- Fetch-time failure warning capture.
- Exhaustive CHECK or generated expression matrices.
- Prepared diagnostics for every unsupported DDL and rollback shape.

## Compatibility Impact

Prepared statements remain partial support, with representative routed
constraint execution failures now covered. CHECK and generated-column support
no longer list prepared diagnostics as a standalone planned gap, though broader
expression and rollback coverage remains partial.

## DDL Metadata Routing Impact

No table-definition metadata changes are introduced. The tests use existing
catalog-backed CHECK and generated-column metadata.

## Single-File And Embedded-Lifecycle Impact

No new companion files or sidecars are introduced. The prepared failures must
not publish rows; existing statement checkpoint and handler rollback coverage
continues to guard visible file state.

## Public API And File-Format Impact

No public `libmylite` API or storage format change is required.

## Storage-Engine Routing Impact

The behavior applies to supported routed engine requests through the shared
prepared execution and MyLite handler path. The smoke coverage uses
`ENGINE=InnoDB` routed tables because that is the primary application-schema
compatibility target.

## Binary-Size And Dependency Impact

No dependency is added and no binary-size-sensitive code path changes are
expected. The slice is test and documentation coverage over existing prepared
statement diagnostics.

## Test And Verification Plan

- Extend storage-engine smoke coverage with prepared CHECK and generated
  duplicate-key diagnostics assertions.
- Update compatibility, storage, roadmap, and related slice docs.
- Run format, targeted storage-smoke tests, prepared/check/generated harness
  reports, tidy, full preset tests, shell checks, and `git diff --check`.

## Acceptance Criteria

- Prepared CHECK violation diagnostics expose MariaDB errno, SQLSTATE, message,
  and retained warning row.
- Prepared generated-column unique-key diagnostics expose MariaDB errno,
  SQLSTATE, message, and retained warning row.
- Compatibility docs no longer list prepared diagnostics as planned for CHECK
  and generated columns.

## Risks And Unresolved Questions

- The stable MyLite result code remains `MYLITE_ERROR`; richer constraint
  classification needs a separate API-policy slice.
- This does not cover fetch-time errors, because current tests use
  non-result prepared statements whose failure occurs during execution.

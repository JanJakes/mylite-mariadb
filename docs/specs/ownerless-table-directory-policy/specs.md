# Ownerless Table Directory Policy

## Problem

MyLite's single-directory contract does not allow SQL table options that route
native table data or index files to caller-named filesystem paths. Global
server-surface policy already rejects `DATA DIRECTORY` and `INDEX DIRECTORY`
table options, but ownerless cross-process SQL coverage did not prove the same
rejection while `MYLITE_OPEN_OWNERLESS_RW` is active or verify final ownerless
and native reopens after those blocked DDL shapes.

This leaves a small DDL/file-lifecycle policy gap around external table-file
routes.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` dispatches `SQLCOM_CREATE_TABLE` and
  `SQLCOM_ALTER_TABLE` to table DDL execution.
- `mariadb/sql/sql_table.cc` parses table create/alter options before native
  engine create/rebuild paths and carries table-option state through
  `mysql_create_table()` and `mysql_alter_table()`.
- `DATA DIRECTORY` and `INDEX DIRECTORY` are MariaDB table options that can
  name paths outside the schema directory. MyLite must reject those options
  before MariaDB reaches native file lifecycle paths.

## Design

Add ownerless SQL coverage that opens a database with
`MYLITE_OPEN_OWNERLESS_RW`, creates a baseline InnoDB table, and verifies these
DDL shapes fail with a MyLite policy error before native mutation:

- `CREATE TABLE ... DATA DIRECTORY='...'`
- `CREATE TABLE ... INDEX DIRECTORY='...'`
- `ALTER TABLE ... DATA DIRECTORY='...'`
- `ALTER TABLE ... INDEX DIRECTORY='...'`
- partition-level `DATA DIRECTORY` inside `CREATE TABLE ... PARTITION BY`

The test then verifies:

- the baseline table remains readable and writable,
- rejected tables do not appear in `INFORMATION_SCHEMA.TABLES`,
- the caller-named external paths were not created,
- final ownerless and native exclusive reopens pass before and after forced
  `.shm` rebuild.

## Scope And Non-Goals

In scope:

- Ownerless rejection evidence for table-level `DATA DIRECTORY` and
  `INDEX DIRECTORY` options.
- Ownerless rejection evidence for a partition-level `DATA DIRECTORY` spelling
  before partition file lifecycle paths can run.
- Final reopen checks for the unaffected baseline table.

Out of scope:

- Supporting external table directories.
- A general ownerless backup/export protocol.
- Durable file lifecycle metadata for supported DDL-created tablespaces.

## Compatibility Impact

No SQL support is added. The slice makes an unsupported surface explicit under
ownerless mode so applications fail before creating native table files outside
the MyLite database directory.

## Directory And Lifecycle Impact

No directory layout changes. The test asserts the caller-named external paths
remain absent after rejected DDL and after ownerless/native reopens.

## Native Storage Impact

No native storage format changes. The point of the policy is to avoid entering
native engine file lifecycle paths for external table directories.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The slice adds test code and documentation.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `table-directory-policy` selector in `embedded-dev`.
- Run the focused selector in `ownerless-test-hooks`.
- Run the embedded ownerless policy-focused selectors.
- Run the hook ownerless policy-focused selectors.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- Ownerless `CREATE TABLE` and `ALTER TABLE` directory-option DDL return a
  MyLite policy error with no MariaDB errno.
- The baseline ownerless table remains writable after rejected DDL.
- Rejected table names stay absent from `INFORMATION_SCHEMA.TABLES`.
- External caller-named directory paths are not created.
- Final ownerless and native exclusive reopen checks pass before and after
  forced `.shm` rebuild.

## Risks And Follow-Up

- This is policy evidence, not support for external table-file routing.
- Broader DDL/file-lifecycle recovery metadata remains planned for supported
  ownerless DDL classes.

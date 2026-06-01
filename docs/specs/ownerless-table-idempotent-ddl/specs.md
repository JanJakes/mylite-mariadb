# Ownerless Table Idempotent DDL

## Problem Statement

Ownerless table lifecycle coverage proves representative create, rename,
truncate, drop, same-name recreate, `CREATE TABLE ... LIKE`, and CTAS refresh
paths. It does not yet prove MariaDB's idempotent table lifecycle spellings:
`CREATE TABLE IF NOT EXISTS` and `DROP TABLE IF EXISTS`.

These spellings are common in application migrations. In ownerless mode they
still cross dictionary-generation and native file-lifecycle boundaries:
already-open peers must observe the table created by another process, must not
see an existing definition rewritten by a duplicate idempotent create, and must
observe final native file absence after repeated idempotent drops.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_yacc.yy`
  - `CREATE TABLE` grammar accepts `opt_if_not_exists`.
  - `DROP TABLE` grammar accepts `opt_if_exists`.
- `mariadb/sql/sql_table.cc`
  - `mysql_create_table_no_lock()` routes an existing table with
    `options.if_not_exists()` to the warning/no-op path instead of returning
    `ER_TABLE_EXISTS_ERROR`.
  - The same function raises `ER_TABLE_EXISTS_ERROR` when the table exists and
    `IF NOT EXISTS` is absent.
  - `mysql_rm_table_no_locks()` emits `ER_BAD_TABLE_ERROR` as a note instead
    of an error when unknown tables are dropped with `IF EXISTS`.
- `mariadb/libmariadb/include/mysqld_error.h`
  - `ER_TABLE_EXISTS_ERROR` is MariaDB errno 1050.
- `packages/libmylite/src/database.cc`
  - Ownerless dictionary DDL classification treats statements whose first token
    is `CREATE` or `DROP` as dictionary-generation boundaries, so idempotent
    table lifecycle statements should refresh already-open peers.

## Design

Add a focused selector, `table-idempotent-ddl`, to
`mylite_ownerless_cross_process_sql_test`.

The selector starts an ownerless parent handle and a child ownerless DDL
process:

1. The child creates an InnoDB table with `CREATE TABLE IF NOT EXISTS` and
   inserts one row.
2. The parent observes the `.frm`/`.ibd` files, table metadata, and rows, then
   verifies plain duplicate `CREATE TABLE` returns MariaDB errno 1050.
3. The parent inserts a row through the already-open handle.
4. The child repeats `CREATE TABLE IF NOT EXISTS` for the same table name with
   a different definition that includes an extra `note` column.
5. The parent verifies the extra column is absent and DML still uses the
   original definition, proving the duplicate idempotent create did not rewrite
   metadata or native files.
6. The child runs `DROP TABLE IF EXISTS` for a missing table. The parent
   verifies the real table and files remain usable.
7. The child drops the real table with `DROP TABLE IF EXISTS`, then repeats the
   same drop as a no-op. The parent verifies metadata, SQL access, `.frm`, and
   `.ibd` absence.
8. Final checks reopen the directory ownerless and native before and after
   forced `.shm` rebuild, verify the table remains absent, recreate and drop it
   once, and leave it absent.

## Scope

In scope:

- SQL-level ownerless coverage for `CREATE TABLE IF NOT EXISTS`.
- Plain duplicate `CREATE TABLE` rejection with MariaDB errno 1050.
- Duplicate `CREATE TABLE IF NOT EXISTS` definition-preservation behavior.
- SQL-level ownerless coverage for `DROP TABLE IF EXISTS` over missing and
  existing table names.
- Native `.frm` and `.ibd` presence/absence checks inside the MyLite database
  directory.
- Compatibility and cross-process-concurrency documentation updates.

Out of scope:

- Temporary table idempotency.
- Partitioned tables and external `DATA DIRECTORY`/`INDEX DIRECTORY` table
  paths, which remain rejected in ownerless mode.
- `CREATE OR REPLACE TABLE` and CTAS idempotency variants.
- Crash/fault injection during table create/drop.

## Compatibility Impact

No intended SQL behavior change. The slice expands ownerless compatibility
evidence for MariaDB idempotent table lifecycle semantics while keeping broader
DDL file-lifecycle recovery partial.

## Directory And Lifecycle Impact

The selector exercises native InnoDB `.frm` and `.ibd` file creation and
removal inside the MyLite-owned database directory. It does not add new durable
paths or change directory layout. Final checks verify absence through
ownerless/native reopen before and after volatile shared-memory rebuild.

## Native Storage Impact

The table is InnoDB. The selector exercises native table creation and drop, but
does not change InnoDB file formats, redo/page-version replay semantics, or
tablespace ownership rules.

## Binary Size Impact

Test and documentation only unless a bug fix is required. No dependency, public
API, or default runtime feature is added.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `table-idempotent-ddl` selector in `embedded-dev`.
- Run adjacent table lifecycle selectors in `embedded-dev`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run adjacent table lifecycle selectors in `ownerless-test-hooks`.
- Run the registered ownerless cross-process SQL CTest filters after
  implementation.
- Run `format-check`, `git diff --check`, and cached diff checks before
  commit.

## Acceptance Criteria

- An already-open ownerless peer sees an InnoDB table created by another
  process through `CREATE TABLE IF NOT EXISTS`.
- Plain duplicate `CREATE TABLE` returns MariaDB errno 1050.
- Duplicate `CREATE TABLE IF NOT EXISTS` preserves the original table
  definition.
- `DROP TABLE IF EXISTS` for a missing table leaves the existing table and
  native files usable.
- Repeated `DROP TABLE IF EXISTS` for the real table removes it once and then
  succeeds as a no-op.
- Final table absence survives ownerless/native reopen before and after forced
  `.shm` rebuild.
- Docs continue to mark broader DDL/file-lifecycle recovery and crash
  injection as planned.

## Risks And Open Questions

- This slice proves bounded InnoDB table create/drop idempotency. It does not
  cover crash recovery inside native table create/drop.
- Broader DDL/file-lifecycle recovery still needs durable metadata for
  unbounded native tablespace detach, import, partition, and crash-in-action
  cases.

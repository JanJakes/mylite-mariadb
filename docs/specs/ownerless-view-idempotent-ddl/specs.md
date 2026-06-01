# Ownerless View Idempotent DDL

## Problem Statement

Ownerless view coverage proves simple create/query/drop, `CREATE OR REPLACE
VIEW`, `ALTER VIEW`, updatable check-option behavior, and nested check-option
refresh. It does not yet prove MariaDB's idempotent view lifecycle spellings:
`CREATE VIEW IF NOT EXISTS` and `DROP VIEW IF EXISTS`.

These spellings matter for application migrations because a duplicate
idempotent create should be a no-op, not a view-definition replacement.
Already-open ownerless peers must observe the created view, preserve the
original definition after duplicate idempotent create, and observe final view
absence after repeated idempotent drops while the base table remains durable.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_yacc.yy`
  - `CREATE VIEW` grammar accepts `opt_if_not_exists` before `table_ident`.
  - `DROP VIEW` grammar accepts `opt_if_exists`.
- `mariadb/sql/sql_view.cc`
  - `mysql_create_view()` checks `lex->create_info.if_not_exists()` when the
    target view/table exists, pushes `ER_TABLE_EXISTS_ERROR` as a note, and
    returns success without replacing the existing view.
  - The same path raises `ER_TABLE_EXISTS_ERROR` for non-idempotent duplicate
    `CREATE VIEW`.
- `mariadb/sql/sql_table.cc`
  - `mysql_rm_table_no_locks()` emits missing object errors as notes when
    `IF EXISTS` is present.
- `mariadb/libmariadb/include/mysqld_error.h`
  - `ER_TABLE_EXISTS_ERROR` is MariaDB errno 1050.
- `packages/libmylite/src/database.cc`
  - Ownerless dictionary DDL classification treats statements whose first token
    is `CREATE` or `DROP` as dictionary-generation boundaries, so idempotent
    view lifecycle statements should refresh already-open peers.

## Design

Add a focused selector, `view-idempotent-ddl`, to
`mylite_ownerless_cross_process_sql_test`.

The selector starts an ownerless parent handle and a child ownerless DDL
process:

1. The child creates an InnoDB base table and creates a view with
   `CREATE VIEW IF NOT EXISTS`.
2. The parent observes the view `.frm`, `INFORMATION_SCHEMA.VIEWS` metadata,
   and query results through the already-open handle.
3. The parent verifies plain duplicate `CREATE VIEW` returns MariaDB errno
   1050, then inserts another base-table row.
4. The child repeats `CREATE VIEW IF NOT EXISTS` for the same view name with a
   different definition. The parent verifies query results still match the
   original definition.
5. The child runs `DROP VIEW IF EXISTS` for a missing view. The parent verifies
   the real view remains queryable.
6. The child drops the real view with `DROP VIEW IF EXISTS`, then repeats the
   same drop as a no-op. The parent verifies view metadata and `.frm` absence
   while the base table remains usable.
7. Final checks reopen the directory ownerless and native before and after
   forced `.shm` rebuild, verify the view remains absent and base rows remain
   durable, recreate/drop the view once, and leave it absent.

## Scope

In scope:

- SQL-level ownerless coverage for `CREATE VIEW IF NOT EXISTS`.
- Plain duplicate `CREATE VIEW` rejection with MariaDB errno 1050.
- Duplicate `CREATE VIEW IF NOT EXISTS` definition-preservation behavior.
- SQL-level ownerless coverage for `DROP VIEW IF EXISTS` over missing and
  existing view names.
- Native view `.frm` presence/absence checks inside the MyLite database
  directory.
- Compatibility and cross-process-concurrency documentation updates.

Out of scope:

- `CREATE OR REPLACE VIEW` and `ALTER VIEW` behavior, already covered by the
  existing view variant selector.
- Check-option semantics, already covered by existing view check-option
  selectors.
- Crash/fault injection during view create/drop.

## Compatibility Impact

No intended SQL behavior change. The slice expands ownerless compatibility
evidence for MariaDB idempotent view lifecycle semantics while keeping broader
view semantics and crash recovery partial.

## Directory And Lifecycle Impact

The selector exercises native view `.frm` file creation and removal inside the
MyLite-owned database directory. It does not add new durable paths or change
directory layout. Final checks verify view absence and base-table durability
through ownerless/native reopen before and after volatile shared-memory
rebuild.

## Native Storage Impact

The base table is InnoDB. The view itself is MariaDB metadata. The selector does
not change InnoDB file formats, redo/page-version replay semantics, or page
ownership rules.

## Binary Size Impact

Test and documentation only unless a bug fix is required. No dependency, public
API, or default runtime feature is added.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `view-idempotent-ddl` selector in `embedded-dev`.
- Run adjacent view selectors in `embedded-dev`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run adjacent view selectors in `ownerless-test-hooks`.
- Run the registered ownerless cross-process SQL CTest filters after
  implementation.
- Run `format-check`, `git diff --check`, and cached diff checks before
  commit.

## Acceptance Criteria

- An already-open ownerless peer sees a view created by another process through
  `CREATE VIEW IF NOT EXISTS`.
- Plain duplicate `CREATE VIEW` returns MariaDB errno 1050.
- Duplicate `CREATE VIEW IF NOT EXISTS` preserves the original definition.
- `DROP VIEW IF EXISTS` for a missing view leaves the existing view usable.
- Repeated `DROP VIEW IF EXISTS` for the real view removes it once and then
  succeeds as a no-op.
- Final view absence and base-table rows survive ownerless/native reopen before
  and after forced `.shm` rebuild.
- Docs continue to mark broader view semantics and crash recovery work as
  planned.

## Risks And Open Questions

- This slice proves bounded idempotent view lifecycle behavior. It does not
  cover every view algorithm, security, column-list, or check-option variant.
- Crash injection inside view metadata file creation/drop remains broader DDL
  fault work.

# Ownerless Column Idempotent DDL

## Problem Statement

Ownerless column ALTER coverage proves representative add, modify, rename,
drop, default, generated-column, and instant-column refresh paths. It does not
yet prove MariaDB's idempotent table-element ALTER paths where duplicate column
adds should be no-ops with `IF NOT EXISTS` and missing column drops should be
no-ops with `IF EXISTS`.

These paths matter for ownerless concurrency because a no-op ALTER statement
still enters the dictionary DDL boundary. Already-open peers must keep their
table definition coherent, preserve the original column default after a
duplicate idempotent add, and observe column removal after an idempotent drop.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_yacc.yy`
  - `add_column` accepts `ADD opt_column opt_if_not_exists_table_element`, so
    `ALTER TABLE ... ADD COLUMN IF NOT EXISTS ...` is parsed.
  - `DROP opt_column opt_if_exists_table_element field_ident` creates
    `Alter_drop::COLUMN` with the parsed `drop_if_exists` flag.
- `mariadb/sql/sql_table.cc`
  - `handle_if_exists_options()` removes an `ADD COLUMN IF NOT EXISTS` item
    when the column already exists and pushes `ER_DUP_FIELDNAME` as a note.
  - The same function removes duplicate add-list entries with the same
    idempotent path.
  - `handle_if_exists_options()` removes a `DROP COLUMN IF EXISTS` item when
    the column is absent and pushes `ER_CANT_DROP_FIELD_OR_KEY` as a note.
  - `mysql_alter_table()` calls `handle_if_exists_options()` before executing
    the remaining table changes.
- `mariadb/libmariadb/include/mysqld_error.h`
  - `ER_DUP_FIELDNAME` is MariaDB errno 1060.
- `packages/libmylite/src/database.cc`
  - Ownerless dictionary DDL classification treats `ALTER TABLE` as a
    dictionary-generation boundary, so already-open peers should refresh around
    idempotent column ALTER statements.

## Design

Add a focused selector, `column-idempotent-ddl`, to
`mylite_ownerless_cross_process_sql_test`.

The selector starts an ownerless parent handle and a child ownerless DDL
process:

1. The child creates an InnoDB table with `id` and `value`.
2. The child adds `note INT NOT NULL DEFAULT 7` with
   `ADD COLUMN IF NOT EXISTS`.
3. The parent observes the new column and default through
   `INFORMATION_SCHEMA.COLUMNS`, inserts a row that receives default `7`, and
   verifies plain duplicate `ADD COLUMN` returns MariaDB errno 1060.
4. The child repeats `ADD COLUMN IF NOT EXISTS note INT NOT NULL DEFAULT 99`.
   The parent verifies the existing default remains `7` and a new row receives
   `7`, proving the duplicate idempotent add did not replace metadata.
5. The child runs `DROP COLUMN IF EXISTS missing_note`. The parent verifies the
   real `note` column still exists and remains writable.
6. The child drops `note` with `DROP COLUMN IF EXISTS`, then repeats the same
   drop as a no-op. The parent verifies the column is absent, later table DML
   works without it, and final ownerless/native reopen before and after forced
   `.shm` rebuild sees the table rows and column absence.

## Scope

In scope:

- SQL-level ownerless coverage for `ADD COLUMN IF NOT EXISTS`.
- Plain duplicate `ADD COLUMN` rejection with MariaDB errno 1060.
- SQL-level ownerless coverage for `DROP COLUMN IF EXISTS` over missing and
  existing column names.
- Already-open peer refresh for idempotent column add/drop metadata.
- Default preservation after duplicate idempotent add.
- Compatibility and cross-process-concurrency documentation updates.

Out of scope:

- New production dictionary/storage code unless the selector exposes a bug.
- Idempotent `CHANGE`, `MODIFY`, `ALTER COLUMN`, `RENAME COLUMN`, index, foreign
  key, CHECK, period, or partition table-element variants.
- Crash/fault injection during ALTER table rewrite.

## Compatibility Impact

No intended SQL behavior change. The slice expands ownerless compatibility
evidence for MariaDB idempotent column ALTER semantics while keeping broader
ALTER TABLE edge cases partial.

## Directory And Lifecycle Impact

No directory layout changes. The selector exercises native InnoDB table
metadata and data files inside the MyLite-owned database directory. Final
checks verify the table remains durable and the dropped column remains absent
through ownerless/native reopen before and after volatile shared-memory
rebuild.

## Native Storage Impact

The table is InnoDB. The selector exercises native ALTER TABLE add/drop column
paths and DML using the changed definition, but it does not change InnoDB file
formats, redo/page-version replay semantics, or page ownership rules.

## Binary Size Impact

Test and documentation only unless a bug fix is required. No dependency,
public API, or default runtime feature is added.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `column-idempotent-ddl` selector in `embedded-dev`.
- Run adjacent column ALTER selectors in `embedded-dev`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run adjacent column ALTER selectors in `ownerless-test-hooks`.
- Run the registered ownerless cross-process SQL CTest filters after
  implementation.
- Run `format-check`, `git diff --check`, and cached diff checks before
  commit.

## Acceptance Criteria

- An already-open ownerless peer sees a column added by another process through
  `ADD COLUMN IF NOT EXISTS`.
- Plain duplicate `ADD COLUMN` returns MariaDB errno 1060.
- Duplicate `ADD COLUMN IF NOT EXISTS` preserves the original column default.
- `DROP COLUMN IF EXISTS` for a missing column leaves the existing column
  usable.
- Repeated `DROP COLUMN IF EXISTS` for the real column removes it once and then
  succeeds as a no-op.
- Final column absence and table rows survive ownerless/native reopen before
  and after forced `.shm` rebuild.
- Docs continue to mark broader ALTER TABLE edge cases and crash recovery as
  planned.

## Risks And Open Questions

- This slice proves bounded column add/drop idempotency. It does not cover all
  idempotent table-element grammar or crash recovery during native ALTER TABLE
  rewrite.
- Online algorithm variants and randomized DDL oracles remain planned broader
  ALTER TABLE work.

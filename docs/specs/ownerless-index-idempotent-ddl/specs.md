# Ownerless Index Idempotent DDL

## Problem Statement

Ownerless standalone index coverage proves representative `CREATE INDEX` and
`DROP INDEX` visibility from another process, plus unique, rename, and ignored
index variants. It does not yet prove MariaDB's idempotent standalone index DDL
paths where duplicate index creation should be a no-op with `IF NOT EXISTS` and
missing index drops should be a no-op with `IF EXISTS`.

These paths matter for ownerless concurrency because index DDL is a dictionary
generation boundary even when MariaDB removes the duplicate or missing-index
operation before native execution. Already-open peers must keep index metadata
coherent, preserve the original index definition after a duplicate idempotent
create, and observe final index absence after repeated idempotent drops.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_yacc.yy`
  - Top-level `CREATE INDEX` grammar accepts `opt_if_not_exists`.
  - Top-level `DROP INDEX` grammar accepts `opt_if_exists_table_element`.
  - Inline and `ALTER TABLE ... ADD/DROP INDEX` table-element grammar also
    carries the same idempotency flags, but this slice is scoped to the
    standalone spelling.
- `mariadb/sql/sql_table.cc`
  - `handle_if_exists_options()` removes an `ADD KEY IF NOT EXISTS` item when
    the table already has an index with the target name and pushes
    `ER_DUP_KEYNAME` as a note.
  - The same function removes duplicate add-list entries with the same
    idempotent path.
  - `handle_if_exists_options()` removes a `DROP INDEX IF EXISTS` item when
    the index is absent and pushes `ER_CANT_DROP_FIELD_OR_KEY` as a note.
  - `CREATE INDEX` and `DROP INDEX` are mapped to `ALTER TABLE` execution, so
    they use the same table-definition and ownerless dictionary boundary.
- `mariadb/libmariadb/include/mysqld_error.h`
  - `ER_DUP_KEYNAME` is MariaDB errno 1061.
- `packages/libmylite/src/database.cc`
  - Ownerless dictionary DDL classification treats statements whose first token
    is `CREATE` or `DROP` as dictionary-generation boundaries, so standalone
    index statements should refresh already-open peers.

## Design

Add a focused selector, `index-idempotent-ddl`, to
`mylite_ownerless_cross_process_sql_test`.

The selector starts an ownerless parent handle and a child ownerless DDL
process:

1. The child creates an InnoDB table with `id`, `value`, and `note` columns.
2. The child creates `ownerless_index_idempotent_idx` with
   `CREATE INDEX IF NOT EXISTS` over `value`.
3. The parent observes the index through `INFORMATION_SCHEMA.STATISTICS`, uses
   it with `FORCE INDEX`, and verifies plain duplicate `CREATE INDEX` returns
   MariaDB errno 1061.
4. The child repeats `CREATE INDEX IF NOT EXISTS` for the same name over
   `note`. The parent verifies the index still targets `value`, proving the
   duplicate idempotent create did not replace the existing definition.
5. The child runs `DROP INDEX IF EXISTS` for a missing index. The parent
   verifies the real index still exists and remains usable.
6. The child drops the real index with `DROP INDEX IF EXISTS`, then repeats the
   same drop as a no-op. The parent verifies forced-index use fails, later DML
   works without the index, and final ownerless/native reopen before and after
   forced `.shm` rebuild sees rows and index absence.

## Scope

In scope:

- SQL-level ownerless coverage for standalone `CREATE INDEX IF NOT EXISTS`.
- Plain duplicate `CREATE INDEX` rejection with MariaDB errno 1061.
- SQL-level ownerless coverage for standalone `DROP INDEX IF EXISTS` over
  missing and existing index names.
- Already-open peer refresh for idempotent index create/drop metadata.
- Index-definition preservation after duplicate idempotent create.
- Compatibility and cross-process-concurrency documentation updates.

Out of scope:

- Inline `CREATE TABLE` index idempotency.
- `ALTER TABLE ... ADD/DROP INDEX IF [NOT] EXISTS` table-element spellings.
- Idempotent unique, primary, full-text, spatial, foreign-key, or CHECK index
  variants.
- Crash/fault injection during native index creation or drop.

## Compatibility Impact

No intended SQL behavior change. The slice expands ownerless compatibility
evidence for MariaDB idempotent standalone index DDL semantics while keeping
broader index and ALTER TABLE edge cases partial.

## Directory And Lifecycle Impact

No directory layout changes. The selector exercises native InnoDB index metadata
inside the MyLite-owned database directory. Final checks verify the table
remains durable and the dropped index remains absent through ownerless/native
reopen before and after volatile shared-memory rebuild.

## Native Storage Impact

The table is InnoDB. The selector exercises native secondary-index creation and
drop paths and DML using the changed definition, but it does not change InnoDB
file formats, redo/page-version replay semantics, or page ownership rules.

## Binary Size Impact

Test and documentation only unless a bug fix is required. No dependency, public
API, or default runtime feature is added.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `index-idempotent-ddl` selector in `embedded-dev`.
- Run adjacent index DDL selectors in `embedded-dev`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run adjacent index DDL selectors in `ownerless-test-hooks`.
- Run the registered ownerless cross-process SQL CTest filters after
  implementation.
- Run `format-check`, `git diff --check`, and cached diff checks before
  commit.

## Acceptance Criteria

- An already-open ownerless peer sees an index created by another process
  through `CREATE INDEX IF NOT EXISTS`.
- Plain duplicate `CREATE INDEX` returns MariaDB errno 1061.
- Duplicate `CREATE INDEX IF NOT EXISTS` preserves the original indexed column.
- `DROP INDEX IF EXISTS` for a missing index leaves the existing index usable.
- Repeated `DROP INDEX IF EXISTS` for the real index removes it once and then
  succeeds as a no-op.
- Final index absence and table rows survive ownerless/native reopen before and
  after forced `.shm` rebuild.
- Docs continue to mark broader index, online DDL, and crash recovery work as
  planned.

## Risks And Open Questions

- This slice proves bounded standalone secondary-index idempotency. It does not
  cover all idempotent key grammar or crash recovery during native index
  creation/drop.
- External randomized DDL oracles remain planned broader ownerless DDL work.

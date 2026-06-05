# Ownerless ALTER INDEX Idempotent DDL

## Problem

Ownerless index-idempotent coverage proved top-level
`CREATE INDEX IF NOT EXISTS` and `DROP INDEX IF EXISTS` spellings. MariaDB also
carries the same idempotency flags through `ALTER TABLE ... ADD INDEX IF NOT
EXISTS` and `ALTER TABLE ... DROP INDEX IF EXISTS`. Already-open ownerless peers
need the same metadata refresh and no-op preservation evidence for those table
element spellings.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:key_def` parses
  `key_or_index opt_if_not_exists ...` and stores the idempotency flag through
  `Lex->add_key()`.
- `mariadb/sql/sql_yacc.yy:alter_list_item` parses
  `DROP key_or_index opt_if_exists_table_element field_ident` into an
  `Alter_drop` with the `IF EXISTS` bit.
- `mariadb/sql/sql_table.cc:handle_if_exists_options()` removes duplicate
  `ADD KEY IF NOT EXISTS` and missing `DROP INDEX IF EXISTS` operations before
  native execution, preserving the current table definition.
- MyLite ownerless dictionary DDL treats the `ALTER TABLE` statement as a
  dictionary-generation boundary, so already-open peers must refresh after both
  no-op and mutating ALTER index idempotency paths.

## Design

Extend the existing `index-idempotent-ddl` selector after the current top-level
index drop:

- run `ALTER TABLE ... ADD INDEX IF NOT EXISTS` over `value`,
- verify an already-open peer sees the new index and can force it,
- verify plain duplicate `ALTER TABLE ... ADD INDEX` returns errno 1061,
- run duplicate `ALTER TABLE ... ADD INDEX IF NOT EXISTS` over `note`,
- verify the original `value` definition is preserved and `note` was not used,
- run missing `ALTER TABLE ... DROP INDEX IF EXISTS`,
- verify the real index remains usable,
- run repeated real `ALTER TABLE ... DROP INDEX IF EXISTS`,
- verify the final index absence and row totals remain unchanged through the
  existing ownerless/native reopen checks.

## Compatibility Impact

No new SQL surface is enabled. This expands ownerless evidence for MariaDB
idempotent secondary-index DDL from top-level standalone index syntax to the
equivalent `ALTER TABLE` table-element syntax.

## Directory And Lifecycle Impact

No directory layout changes. The test continues to use native InnoDB secondary
index metadata inside the MyLite database directory and verifies final state
after forced volatile shared-memory rebuild.

## Native Storage Impact

No storage format changes. Mutating ALTER phases create and drop a native InnoDB
secondary index; idempotent no-op phases preserve native metadata.

## Binary Size And Dependencies

No binary-size, dependency, or license changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `index-idempotent-ddl` selector.
- Run the matching embedded ownerless cross-process SQL CTest shard.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- `ALTER TABLE ... ADD INDEX IF NOT EXISTS` creates a peer-visible index.
- Duplicate plain `ALTER TABLE ... ADD INDEX` returns MariaDB errno 1061.
- Duplicate `ALTER TABLE ... ADD INDEX IF NOT EXISTS` preserves the original
  key part.
- Missing `ALTER TABLE ... DROP INDEX IF EXISTS` preserves the real index.
- Repeated real `ALTER TABLE ... DROP INDEX IF EXISTS` leaves final index
  absence through ownerless/native reopen before and after forced `.shm`
  rebuild.

## Risks And Follow-Up

- Inline `CREATE TABLE` index idempotency remains planned. Unique
  secondary-index idempotency is covered by
  `ownerless-unique-index-idempotent-ddl`, primary-key ADD idempotency is
  covered by `ownerless-primary-key-idempotent-ddl`, and idempotent
  FULLTEXT/SPATIAL policy rejection is covered by
  `ownerless-special-index-idempotent-policy`.
- Randomized DDL oracle coverage remains planned broader ownerless work.

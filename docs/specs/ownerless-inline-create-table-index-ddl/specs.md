# Ownerless Inline CREATE TABLE Index DDL

## Problem

MariaDB accepts ordinary inline secondary-index definitions in
`CREATE TABLE`, but inline `IF NOT EXISTS` key definitions are blocked outside
`ALTER TABLE` by `LEX::check_add_key()`. MyLite needs bounded ownerless evidence
for the supported create-table behavior: an inline secondary index created by
one ownerless process is visible and usable from an already-open ownerless peer,
while duplicate inline key names fail with MariaDB duplicate-key diagnostics and
do not leave a table behind.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:key_def` parses inline `CREATE TABLE` key
  definitions and calls `Lex->add_key()`.
- `mariadb/sql/sql_lex.h:LEX::check_add_key()` rejects `IF NOT EXISTS` for key
  definitions unless `sql_command == SQLCOM_ALTER_TABLE`, so inline
  `CREATE TABLE ... INDEX IF NOT EXISTS` is not a supported MariaDB surface in
  this base line even though the grammar token is present.
- `mariadb/sql/sql_table.cc:mysql_prepare_create_table()` builds key metadata
  for a new table and checks duplicate key names with
  `check_if_keyname_exists()`.
- That create-table duplicate-name path raises `ER_DUP_KEYNAME` directly and
  does not call `handle_if_exists_options()`.
- `mariadb/sql/sql_table.cc:handle_if_exists_options()` remains the source of
  duplicate-key no-op behavior for `ALTER TABLE ... ADD INDEX IF NOT EXISTS`
  and standalone `CREATE INDEX IF NOT EXISTS` mapped through ALTER.

## Scope And Non-Goals

- Extend the existing `index-idempotent-ddl` selector with ordinary inline
  secondary-index coverage.
- Verify a child ownerless process can create an InnoDB table with an inline
  secondary index.
- Verify an already-open ownerless peer sees the inline index in
  `information_schema.statistics` and can force it.
- Verify duplicate inline index names in one `CREATE TABLE` fail with MariaDB
  errno 1061.
- Verify the failed duplicate-inline create leaves no application table.
- Do not claim inline `CREATE TABLE ... INDEX IF NOT EXISTS` support.
- Do not add inline unique, primary, full-text, spatial, foreign-key, or CHECK
  variants in this slice.

## Design

The existing `index-idempotent-ddl` child runs its standalone and ALTER-table
secondary-index idempotency phases, then creates
`app.ownerless_inline_index_base` with
`INDEX ownerless_inline_index_idx (value)` and inserts three rows. The parent
verifies index metadata and forced-index reads through an already-open
ownerless handle.

The child then attempts to create `app.ownerless_inline_index_duplicate` with
two inline `INDEX ownerless_inline_duplicate_idx` definitions over different
columns. It expects MariaDB errno 1061 and signals the parent, which verifies
the failed table is absent. Final ownerless/native reopen checks also verify
the successful inline table/index and the failed duplicate table absence.

## Compatibility Impact

This narrows the previous generic "inline CREATE TABLE index idempotency"
follow-up. Ownerless mode now has evidence for MariaDB's supported ordinary
inline secondary-index create syntax and duplicate inline key-name failure
behavior. Inline `CREATE TABLE ... INDEX IF NOT EXISTS` remains unsupported by
MariaDB's key setup path in this base line, and the docs no longer claim it.

## Directory And Lifecycle Impact

No directory layout changes. The successful inline table and index are native
InnoDB metadata inside the MyLite database directory; the duplicate-inline
failure must not leave a table directory entry.

## Native Storage Impact

No storage format changes. The slice exercises native InnoDB table creation
with an inline secondary index.

## Binary Size And Dependencies

No binary-size, dependency, or license changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `index-idempotent-ddl` selector.
- Run the matching embedded ownerless cross-process SQL CTest shard.
- Build and run the focused `index-idempotent-ddl` selector in
  `ownerless-test-hooks`.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- Ordinary inline `INDEX` in `CREATE TABLE` creates peer-visible index metadata.
- An already-open ownerless peer can force the inline-created index.
- Duplicate inline `INDEX` definitions fail with errno 1061.
- The duplicate-inline failure leaves no application table.
- Successful inline table/index metadata and failed-table absence survive
  ownerless/native reopen before and after forced `.shm` rebuild.

## Risks And Follow-Up

- Inline unique, primary, foreign-key, CHECK, generated-column, and special-index
  variants remain broader work unless another focused slice covers them.
- Crash-injected inline `CREATE TABLE` index DDL and randomized DDL oracle
  coverage remain planned broader work.

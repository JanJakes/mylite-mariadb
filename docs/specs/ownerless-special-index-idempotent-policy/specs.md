# Ownerless Special Index Idempotent Policy

## Problem

Ownerless mode rejects `FULLTEXT` and `SPATIAL` index DDL before MariaDB can
enter unproven full-text auxiliary metadata or spatial R-tree paths. MariaDB
also accepts `IF NOT EXISTS` on top-level and table-element special-index
definitions. MyLite needs evidence that idempotent-looking special-index DDL is
still rejected in ownerless read/write mode and does not bypass the fail-closed
policy.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses top-level
  `CREATE FULLTEXT INDEX opt_if_not_exists` and
  `CREATE SPATIAL INDEX opt_if_not_exists` through `Lex->add_create_index()`.
- `mariadb/sql/sql_yacc.yy:key_def` parses table-element
  `FULLTEXT ... opt_if_not_exists` and `SPATIAL ... opt_if_not_exists`
  definitions through `Lex->add_key()`, covering inline `CREATE TABLE` and
  `ALTER TABLE ... ADD` spellings.
- `packages/libmylite/src/database.cc:is_unsupported_ownerless_special_index_statement()`
  rejects ownerless `CREATE` or `ALTER` statements that contain raw unquoted
  `FULLTEXT` or `SPATIAL` tokens, before MariaDB prepares or executes the SQL.

## Scope And Non-Goals

- Extend the existing `special-index-policy` selector.
- Reject ownerless read/write `CREATE FULLTEXT INDEX IF NOT EXISTS` and
  `CREATE SPATIAL INDEX IF NOT EXISTS`.
- Reject ownerless read/write `ALTER TABLE ... ADD FULLTEXT INDEX IF NOT
  EXISTS` and `ALTER TABLE ... ADD SPATIAL INDEX IF NOT EXISTS`.
- Reject ownerless read/write inline `FULLTEXT KEY IF NOT EXISTS` and
  `SPATIAL INDEX IF NOT EXISTS` table definitions.
- Verify rejection remains a MyLite policy error with MariaDB errno zero.
- Verify rejected idempotent spellings leave no index metadata or application
  tables.
- Do not add ownerless support for special indexes or special-index drop
  coordination.

## Design

No policy-code change is expected. The tokenizer already scans all raw tokens
after a leading `CREATE` or `ALTER`; idempotent special-index DDL still contains
raw `FULLTEXT` or `SPATIAL` tokens. The test expands the policy selector with
top-level, ALTER, and inline idempotent spellings and extends the final metadata
absence checks with the new object names.

## Compatibility Impact

Ownerless mode remains fail-closed for `FULLTEXT` and `SPATIAL` index DDL,
including idempotent-looking spellings. This is explicit unsupported behavior,
not partial support for special indexes.

## Directory And Lifecycle Impact

The rejected statements must not create special-index metadata, auxiliary state,
or inline-created application tables in the MyLite database directory.

## Native Storage Impact

No native storage format changes. The slice prevents entry into native
full-text and spatial index paths for additional SQL spellings.

## Binary Size And Dependencies

No binary-size, dependency, or license changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `special-index-policy` selector.
- Run the matching embedded ownerless cross-process SQL CTest shard.
- Build and run the focused `special-index-policy` selector in
  `ownerless-test-hooks`.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- Idempotent top-level `FULLTEXT` and `SPATIAL` index DDL returns a MyLite
  policy error with MariaDB errno zero.
- Idempotent ALTER-add `FULLTEXT` and `SPATIAL` index DDL returns the same
  policy error.
- Inline idempotent `FULLTEXT` and `SPATIAL` table definitions return the same
  policy error and leave no application tables.
- Final state survives ownerless/native reopen before and after forced `.shm`
  rebuild.

## Risks And Follow-Up

- Ownerless special-index support, special-index drop coordination,
  crash-recovery coverage, and external MariaDB/RQG stress remain planned
  broader work.

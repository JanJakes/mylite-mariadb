# Ownerless Schema Idempotent DDL Variants

## Problem

Ownerless schema lifecycle coverage proves ordinary `CREATE DATABASE`,
schema-qualified InnoDB use, `ALTER DATABASE` default rewrites, and
`DROP DATABASE`. It does not yet prove MariaDB's idempotent schema DDL spellings
across ownerless peers: `CREATE SCHEMA IF NOT EXISTS`,
`CREATE DATABASE IF NOT EXISTS`, and `DROP SCHEMA IF EXISTS`.

Those forms are common in application migrations. They should pass through the
same ownerless dictionary-generation boundary as ordinary schema DDL while
preserving MariaDB semantics for no-op duplicate create and no-op absent drop.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/lex.h:577` maps `SCHEMA` to the `DATABASE` token, so the
  `SCHEMA` spellings share the database DDL grammar and command paths.
- `mariadb/sql/sql_yacc.yy:2691` parses
  `CREATE DATABASE opt_if_not_exists ident`, and
  `mariadb/sql/sql_yacc.yy:13474` parses `DROP DATABASE opt_if_exists ident`.
- `mariadb/sql/sql_parse.cc:5145` dispatches `SQLCOM_CREATE_DB` through
  `mysql_create_db()`, and `mariadb/sql/sql_parse.cc:5163` dispatches
  `SQLCOM_DROP_DB` through `mysql_rm_db()`.
- `mariadb/sql/sql_db.cc:748` implements `mysql_create_db_internal()`.
  When the schema directory already exists and `IF NOT EXISTS` is present, the
  function emits a note, sets affected rows to zero, and skips the `db.opt`
  rewrite path.
- `mariadb/sql/sql_db.cc:1059` implements `mysql_rm_db_internal()`. Missing
  schemas with `IF EXISTS` emit a note and return success without table-file
  lifecycle work.
- `packages/libmylite/src/database.cc:7558` classifies `ALTER`, `CREATE`,
  `DROP`, `RENAME`, and `TRUNCATE` as ownerless dictionary DDL, so these no-op
  spellings should still publish a stable ownerless dictionary boundary before
  already-open peers continue.

## Design

Add a focused `schema-idempotent-ddl` selector to
`mylite_ownerless_cross_process_sql_test`:

1. One ownerless child process runs `CREATE SCHEMA IF NOT EXISTS` with explicit
   `latin1` defaults, creates an InnoDB table in that schema, and inserts a
   row.
2. An already-open ownerless peer verifies the schema, `db.opt`, inherited
   column collation, and table rows, then writes another row.
3. The child runs duplicate `CREATE DATABASE IF NOT EXISTS` with different
   defaults and `DROP SCHEMA IF EXISTS` for a missing schema.
4. The peer verifies the original schema defaults remain `latin1`, the missing
   schema is absent, and existing table rows remain readable and writable.
5. The child runs `DROP SCHEMA IF EXISTS` for the existing schema and a second
   idempotent `DROP DATABASE IF EXISTS` for the same now-absent schema.
6. The peer verifies schema/table absence, then final checks reopen the
   directory ownerless and native-exclusive before and after forced `.shm`
   deletion.

## Scope And Non-Goals

In scope:

- `SCHEMA`/`DATABASE` synonym coverage for schema DDL.
- `IF NOT EXISTS` duplicate-create behavior over an existing schema.
- `IF EXISTS` absent-drop behavior and final existing-schema drop behavior.
- Native `datadir/<schema>/db.opt` presence while the schema exists and absence
  after drop.
- Final ownerless/native reopen checks before and after forced `.shm` rebuild.

Out of scope:

- `CREATE OR REPLACE DATABASE`.
- Crash injection inside schema create/drop or `db.opt` handling.
- SQL-level table-lock wait fault injection.
- External randomized DDL/RQG stress.

## Compatibility Impact

No product SQL semantics change. The slice expands ownerless compatibility
evidence for MariaDB-compatible idempotent schema migration statements while
keeping broader schema behavior partial.

## Directory And Lifecycle Impact

No directory layout change. The test checks MariaDB's native schema directory
and `db.opt` file under `datadir/` during the live phase, then verifies the
schema directory is gone after drop and after ownerless/native reopen.

## Native Storage Impact

No native storage format change. The created table is InnoDB so the test also
exercises native table-file removal through MariaDB's `DROP DATABASE` path
behind ownerless dictionary generation.

## Public API Impact

No public API change.

## Binary Size Impact

No production binary-size impact. The slice adds test code and documentation.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `schema-idempotent-ddl` selector in `embedded-dev`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run adjacent schema selectors to catch ordering or shared helper regressions.
- Run ownerless CTest coverage appropriate to the touched SQL selector.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- Already-open ownerless peers see a schema created by `CREATE SCHEMA IF NOT
  EXISTS`.
- Duplicate `CREATE DATABASE IF NOT EXISTS` succeeds without rewriting existing
  schema defaults.
- `DROP SCHEMA IF EXISTS` succeeds for missing and existing schemas.
- Final schema absence and directory removal survive ownerless/native reopen
  before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This does not add crash injection for interrupted schema DDL. Existing
  dictionary-DDL hook tests remain the general crash boundary; schema-specific
  crash points remain a follow-up.
- This does not change the broader DDL/file-lifecycle recovery limitation:
  DDL-created tablespace replay still relies on the conservative native-file
  bridge until durable file lifecycle metadata is designed.

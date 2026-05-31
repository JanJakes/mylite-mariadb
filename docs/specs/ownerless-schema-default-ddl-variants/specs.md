# Ownerless Schema Default DDL Variants

## Problem

Ownerless schema lifecycle coverage currently proves `CREATE DATABASE`,
qualified InnoDB table use, peer-visible `DROP DATABASE`, and final absent
schema state. It does not cover schema option files rewritten by
`ALTER DATABASE`, nor whether an already-open peer observes changed schema
defaults before creating and using new tables in that schema.

This leaves part of the broader schema/file-lifecycle gap untested even though
MariaDB stores schema defaults in the database directory as `db.opt`.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` dispatches `SQLCOM_CREATE_DB`,
  `SQLCOM_ALTER_DB`, and `SQLCOM_DROP_DB` through `mysql_create_db()`,
  `mysql_alter_db()`, and `mysql_rm_db()`.
- `mariadb/sql/sql_db.cc` `mysql_create_db_internal()` locks the schema name,
  creates the schema directory, and writes `MY_DB_OPT_FILE`.
- `mariadb/sql/sql_db.cc` `mysql_alter_db_internal()` locks the schema name and
  recreates the same `db.opt` file with the new default charset/collation.
- `mariadb/sql/sql_db.cc` `mysql_rm_db_internal()` locks the schema name, locks
  objects inside the schema, removes known table files, and removes the schema
  directory.
- `mariadb/sql/sql_show.cc` exposes schema defaults through
  `INFORMATION_SCHEMA.SCHEMATA.DEFAULT_CHARACTER_SET_NAME` and
  `DEFAULT_COLLATION_NAME`.
- MyLite treats ownerless `CREATE`, `ALTER`, `DROP`, `RENAME`, and `TRUNCATE`
  statements as dictionary DDL, so schema default rewrites should pass through
  the same ownerless dictionary generation boundary as covered table DDL.

## Design

Add ownerless SQL coverage where one process performs schema default DDL while
an already-open ownerless peer observes each dictionary boundary:

1. Create a schema with `latin1`/`latin1_swedish_ci` defaults, create an InnoDB
   table inside it, and insert rows.
2. Verify the peer sees the schema defaults through `INFORMATION_SCHEMA`, sees
   the schema `db.opt` file in the MyLite datadir, and can write through the
   table.
3. Alter the schema defaults to `utf8mb4`/`utf8mb4_unicode_ci`.
4. Verify the peer sees the changed schema defaults while the existing table's
   `VARCHAR` column remains `latin1`.
5. Create a second InnoDB table in the altered schema and verify its `VARCHAR`
   column inherits the new `utf8mb4` collation.
6. Drop the schema and verify ownerless/native reopens, including after forced
   `.shm` rebuild, see the schema and tables as absent and the schema directory
   removed.

## Scope And Non-Goals

In scope:

- Ownerless `CREATE DATABASE ... DEFAULT CHARACTER SET/COLLATE` peer refresh.
- Ownerless `ALTER DATABASE ... DEFAULT CHARACTER SET/COLLATE` peer refresh.
- Schema `db.opt` presence while the schema exists and absence after drop.
- Final ownerless/native reopen checks before and after forced `.shm` rebuild.

Out of scope:

- Renaming databases, which MariaDB handles through a separate upgrade path.
- Crash injection inside schema option-file rewrite.
- Full durable file lifecycle metadata for every DDL class.
- External randomized DDL/RQG stress.

## Compatibility Impact

SQL semantics are unchanged. The slice expands ownerless compatibility evidence
for MariaDB schema default behavior that applications can observe through
`INFORMATION_SCHEMA` and inherited table column collations.

## Directory And Lifecycle Impact

No directory layout changes. The test explicitly checks MariaDB's native
schema directory and `db.opt` file stay inside `datadir/` while the schema
exists and that the schema directory is gone after `DROP DATABASE`.

## Native Storage Impact

No native storage format changes. The schema option-file rewrite is MariaDB
native behavior; MyLite only coordinates the ownerless dictionary generation
and peer refresh around it.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The slice adds test code and documentation.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `schema-default-ddl` selector in `embedded-dev`.
- Run the focused selector in `ownerless-test-hooks`.
- Run full embedded ownerless cross-process SQL CTest coverage.
- Run full hook ownerless cross-process SQL CTest coverage.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The already-open peer observes the initial and altered schema defaults.
- The table created before `ALTER DATABASE` keeps its original column charset.
- The table created after `ALTER DATABASE` inherits the new schema defaults.
- The schema `db.opt` file exists while the schema exists and is gone after
  `DROP DATABASE`.
- Final ownerless and native exclusive reopen checks pass before and after
  forced `.shm` rebuild.

## Risks And Follow-Up

- This does not add crash injection for an interrupted `db.opt` rewrite.
  Dictionary DDL crash hooks remain broader coverage, and schema-specific
  option-file crash injection remains planned if this path becomes a support
  boundary.
- The slice still does not provide durable file lifecycle metadata for all DDL
  classes; it only closes a bounded schema-default gap.

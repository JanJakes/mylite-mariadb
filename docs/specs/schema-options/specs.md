# Schema Options

## Problem

MyLite now persists schema names, but MariaDB stores schema defaults in the
transient runtime `db.opt` file. A schema created with a default character set,
collation, or comment can be reopened by name, but its defaults are lost unless
MyLite mirrors those options into the `.mylite` catalog.

This slice persists the schema options that MariaDB exposes through
`INFORMATION_SCHEMA.SCHEMATA`.

## Source Findings

Base authority: MariaDB 11.8.6, initial import ref
`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.

- `mariadb/sql/sql_db.cc` writes `default-character-set`,
  `default-collation`, and `comment` to `db.opt` in `write_db_opt()`.
- `load_db_opt()` reads those values back into `Schema_specification_st`, using
  `thd->variables.collation_server` when no options file exists.
- `mysql_create_db_internal()` and `mysql_alter_db_internal()` update `db.opt`
  after successful `CREATE/ALTER DATABASE`.
- `mariadb/sql/sql_show.cc` renders `SHOW CREATE DATABASE` and fills
  `INFORMATION_SCHEMA.SCHEMATA` from `load_db_opt_by_name()`, including
  `DEFAULT_CHARACTER_SET_NAME`, `DEFAULT_COLLATION_NAME`, and
  `SCHEMA_COMMENT`.

## Design

- Extend MyLite schema catalog records to use the existing variable record
  fields after the schema name for:
  - default character set name,
  - default collation name,
  - schema comment.
- Add storage APIs to store and read schema definitions with those options.
- Change schema catalog sync to read runtime schema definitions from
  `INFORMATION_SCHEMA.SCHEMATA` instead of only `SHOW DATABASES`.
- Treat successful direct and prepared `ALTER DATABASE` / `ALTER SCHEMA` as
  schema-catalog sync points, alongside create/drop.
- Rehydrate transient runtime directories with `CREATE DATABASE IF NOT EXISTS`
  plus the stored character set, collation, and comment.

## Compatibility Impact

Covered in this slice:

- Direct and prepared schema sync preserves default character set, default
  collation, and comment after successful create or alter.
- File-backed reopen reconstructs transient runtime schema options before user
  SQL, so `INFORMATION_SCHEMA.SCHEMATA` and `SHOW CREATE DATABASE` see the
  stored defaults.

Still planned:

- A final SQL-layer database hook that avoids the temporary directory bridge.
- SQL transaction rollback semantics for schema DDL.
- Broader schema-option edge cases beyond MariaDB's basic charset, collation,
  and comment defaults.

## Single-File And Embedded Lifecycle

Durable schema options live in the primary `.mylite` catalog. Runtime `db.opt`
files remain transient MyLite-owned state inside the temporary runtime
directory and are regenerated from the catalog on file-backed open.

## Public API And File Format

The public `libmylite` API does not change. The internal storage API gains
schema-definition store/read helpers. The catalog record layout is not widened;
schema records reuse the existing variable fields that table records use for
table and engine metadata.

## Test Plan

- Extend storage unit coverage for schema definitions with charset, collation,
  and comment.
- Extend storage-engine smoke coverage for create, alter, catalog readback,
  close/reopen rehydration, information-schema visibility, and drop cleanup.
- Run storage, embedded, storage-smoke, formatting, tidy, compatibility report,
  and size checks.

## Acceptance Criteria

- Schema options survive close/reopen without durable MariaDB datadir state.
- Direct and prepared schema catalog sync points continue to pass existing
  namespace tests.
- Compatibility, roadmap, API, and storage architecture docs no longer list
  basic schema options as a remaining schema namespace gap.

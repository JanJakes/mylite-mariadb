# CREATE DATABASE Existence Options

## Goal

Make catalog-backed schemas behave like MariaDB schemas when `CREATE DATABASE`
is executed after a file-backed MyLite close/reopen without a runtime schema
directory. Existing catalog-only schemas must be visible to plain
`CREATE DATABASE`, `CREATE DATABASE IF NOT EXISTS`, and
`CREATE OR REPLACE DATABASE`.

## Non-Goals

- Do not make initial `CREATE DATABASE` fully filesystem-free; active
  connections may still use MyLite-owned transient schema directories for the
  first create.
- Do not add `CREATE OR REPLACE SCHEMA` syntax coverage unless MariaDB already
  accepts it through the same parser path.
- Do not implement transactional schema DDL, savepoint rollback, views,
  triggers, routines, events, or other non-table database object cataloging.
- Do not claim broader `CREATE OR REPLACE TABLE` semantics.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:2691-2701` parses
  `CREATE [OR REPLACE] DATABASE [IF NOT EXISTS] name` and stores the DDL
  options in `LEX::create_info`.
- `mariadb/sql/sql_parse.cc:5100-5115` dispatches `SQLCOM_CREATE_DB` to
  `mysql_create_db()` and requires `DROP_ACL` when `OR REPLACE` is present.
- `mariadb/sql/sql_db.cc:786-819` detects existing schemas only by runtime
  directory before applying `OR REPLACE`, `IF NOT EXISTS`, or duplicate-create
  errors.
- `mariadb/sql/sql_db.cc:920-922` already routes `ALTER DATABASE` over
  catalog-only MyLite schemas to `mysql_alter_mylite_db()`.
- `mariadb/sql/sql_db.cc:1381-1435` already routes `DROP DATABASE` over
  catalog-only MyLite schemas to `mysql_rm_mylite_db()`.
- `packages/libmylite/src/database.cc:2551-2567` treats
  `CREATE OR REPLACE DATABASE` as schema-catalog SQL, so successful statements
  are followed by catalog sync.

## Compatibility Impact

Covered by this slice:

- Plain `CREATE DATABASE existing_name` over a catalog-only schema fails like
  MariaDB duplicate-create behavior and does not create a runtime directory.
- `CREATE DATABASE IF NOT EXISTS existing_name` over a catalog-only schema
  succeeds with a note-level warning and preserves schema options, tables, and
  rows.
- `CREATE OR REPLACE DATABASE existing_name ...` over a catalog-only schema
  drops existing MyLite catalog schema contents, stores the new schema options,
  leaves no runtime schema directory, and survives close/reopen.

The support claim remains partial because initial schema creation still uses
the transient MariaDB directory path during the active connection, and broader
database-object cataloging plus transactional schema DDL remain planned.

## Proposed Design

Add a narrow `mysql_create_mylite_db()` branch beside the existing
`mysql_alter_mylite_db()` and `mysql_rm_mylite_db()` hooks. In
`mysql_create_db_internal()`, after the normalized schema name is locked and the
runtime schema path is built:

1. If MyLite schema hooks are active, the catalog schema exists, and the runtime
   schema directory does not exist, route to `mysql_create_mylite_db()`.
2. For `OR REPLACE`, reuse `mysql_rm_mylite_db(..., silent=true)` to remove the
   old catalog schema and its tables, reset diagnostics, then store the new
   schema options through `mylite_schema_store_options()`.
3. For `IF NOT EXISTS`, emit MariaDB's duplicate-schema note and report zero
   affected rows without mutating the catalog.
4. For plain duplicate `CREATE DATABASE`, raise `ER_DB_CREATE_EXISTS`.
5. Preserve MariaDB-style DDL logging and binlog behavior for the successful
   catalog-backed create path.

Missing schemas continue through MariaDB's existing directory-backed create path
and are synchronized into the MyLite catalog by `libmylite` after execution.

## Affected Subsystems

- `mariadb/sql/sql_db.cc` database-create path.
- MyLite schema hook registry through existing `mylite_schema_store_options()`
  and `mylite_schema_drop()` callbacks.
- `packages/libmylite/tests/embedded_storage_engine_test.c` storage-smoke
  schema namespace coverage.

## DDL Metadata Routing Impact

Catalog-only `CREATE OR REPLACE DATABASE` must mutate only the MyLite catalog
in the primary `.mylite` file. Replacement must remove schema table records as
well as the schema record before storing the new schema options.

## Single-File And Lifecycle Impact

No new companion file type is introduced. The tests must prove that catalog-only
create existence handling does not recreate a runtime schema directory and does
not publish forbidden durable sidecars.

## Public API And File-Format Impact

No public C API or storage file-format change is expected. The slice uses the
existing schema catalog record and storage drop-schema behavior.

## Storage-Engine Routing Impact

Tables under the replaced schema are routed MyLite catalog records and must be
removed by the existing schema-drop storage path. Requested table engines are not
changed.

## Binary-Size, License, And Dependency Impact

No dependency or license change is expected. Binary-size impact should be a
small MariaDB SQL-layer branch and storage-smoke test code.

## Test Plan

1. Create a schema with options and a routed `ENGINE=InnoDB` table.
2. Close and reopen, asserting no runtime schema directory exists.
3. Assert plain `CREATE DATABASE existing_name` fails without recreating the
   runtime schema directory or dropping old data.
4. Assert `CREATE DATABASE IF NOT EXISTS existing_name ...` succeeds with a
   warning, preserves old options and rows, and keeps no runtime schema
   directory.
5. Assert `CREATE OR REPLACE DATABASE existing_name ...` succeeds, keeps no
   runtime schema directory, removes old table records and rows, stores the new
   schema options, and leaves the schema usable.
6. Close/reopen again and assert the replacement schema options and empty table
   list survive.
7. Run focused storage-smoke tests, schema/DDL/sidecar harness reports,
   formatting, shell checks, clang-tidy, and full `dev`, `embedded-dev`, and
   `storage-smoke-dev` gates.

## Acceptance Criteria

- Duplicate plain `CREATE DATABASE` over a catalog-only MyLite schema fails and
  leaves existing schema state intact.
- `CREATE DATABASE IF NOT EXISTS` over a catalog-only MyLite schema warns and
  leaves existing schema state intact.
- `CREATE OR REPLACE DATABASE` over a catalog-only MyLite schema replaces the
  schema catalog contents without recreating a runtime schema directory.
- Replacement options are visible through `INFORMATION_SCHEMA.SCHEMATA` before
  and after close/reopen.
- Durable sidecar gates pass.

## Implementation Status

Implemented:

- `mysql_create_db_internal()` routes existing catalog-only MyLite schemas to a
  MyLite create helper when no runtime schema directory exists.
- Plain duplicate `CREATE DATABASE` fails without creating a runtime schema
  directory.
- `CREATE DATABASE IF NOT EXISTS` warns without mutating schema options or
  routed table contents.
- `CREATE OR REPLACE DATABASE` drops the catalog schema contents, stores the new
  schema options, and keeps the replacement catalog-only before and after
  close/reopen.
- Storage-smoke coverage verifies table removal, option replacement,
  information-schema visibility, close/reopen discovery, and sidecar gates.

Measured rebuilt MariaDB embedded archive sizes:

| Build | Size |
| --- | ---: |
| `tools/mariadb-embedded-build all` | 30.57 MiB |
| `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC` | 30.71 MiB |

## Risks And Unresolved Questions

- Initial `CREATE DATABASE` still creates a transient runtime schema directory
  until a later slice removes that active-connection dependency.
- Failed schema replacement rollback stays bounded to the existing outer
  statement checkpoint and is not full transactional DDL.

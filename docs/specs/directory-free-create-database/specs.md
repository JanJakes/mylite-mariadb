# Directory-Free CREATE DATABASE

## Goal

Make initial file-backed `CREATE DATABASE` and `CREATE SCHEMA` publish schema
metadata directly to the MyLite catalog, without creating a transient runtime
schema directory during the active connection.

## Non-Goals

- Do not change `:memory:` behavior; in-memory sessions do not pass a MyLite
  primary file to MariaDB and continue using MariaDB's normal transient runtime
  schema path.
- Do not implement catalog-backed views, triggers, routines, events, sequences,
  partition metadata, or foreign-key enforcement.
- Do not claim transactional schema DDL beyond the existing statement
  checkpoint coverage.
- Do not change the public `libmylite` C API or storage file format.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:2691-2701` parses
  `CREATE [OR REPLACE] DATABASE|SCHEMA [IF NOT EXISTS] name` into the database
  create command handled by `mysql_create_db()`.
- `mariadb/sql/sql_db.cc:763-908` implements
  `mysql_create_db_internal()` by checking the schema directory under
  `mysql_data_home`, creating that directory, and writing `.db.opt`.
- Before this slice, `mariadb/sql/sql_db.cc:912-967` handled only existing
  catalog-only MyLite schemas for duplicate, `IF NOT EXISTS`, and
  `OR REPLACE` create paths.
- `mariadb/sql/sql_db.cc:685-704` loads schema options from the MyLite catalog
  when `.db.opt` is absent, through `mylite_schema_load_options()`.
- `mariadb/sql/sql_db.cc:2349-2352` and
  `mariadb/sql/sql_show.cc:5799-5801` already treat MyLite catalog schemas as
  existing schemas for `USE`, `SHOW CREATE DATABASE`, and
  `INFORMATION_SCHEMA.SCHEMATA`.
- `mariadb/sql/sql_show.cc:4529-4565` and `mariadb/sql/sql_show.cc:4804-4808`
  add MyLite catalog schema and table names to `SHOW DATABASES`, `SHOW TABLES`,
  and information-schema discovery when runtime directories are absent.
- `packages/libmylite/src/database.cc:2942-2945` does not pass
  `--mylite-primary-file` for `:memory:`, so the SQL-layer schema hooks are
  inactive for in-memory databases.

## Compatibility Impact

Covered by this slice:

- File-backed initial `CREATE DATABASE` and `CREATE SCHEMA` create a MyLite
  catalog schema without creating a runtime schema directory.
- `CREATE DATABASE IF NOT EXISTS missing_schema` and
  `CREATE OR REPLACE DATABASE missing_schema` keep MariaDB-style success
  behavior for missing schemas.
- Immediate `USE`, `SHOW DATABASES`, `SHOW CREATE DATABASE`,
  `INFORMATION_SCHEMA.SCHEMATA`, table creation, insert/select, and close/reopen
  discovery work over the catalog-only schema.
- `:memory:` keeps the existing MariaDB path and remains isolated from the
  MyLite storage handler.

Still planned:

- Catalog-backed non-table objects.
- Full transactional schema DDL and multi-statement rollback.
- Broader object-specific SQL-layer filesystem paths.

## Proposed Design

Broaden the existing `mysql_create_mylite_db()` helper so it accepts whether
the schema already exists in the MyLite catalog.

In `mysql_create_db_internal()`, after the schema MDL lock is acquired and the
runtime schema path is computed:

1. When MyLite schema hooks are active and the runtime schema directory is
   absent, route to `mysql_create_mylite_db()`.
2. If the schema is missing, store the requested schema options directly through
   `mylite_schema_store_options()`, report one affected row, and preserve
   MariaDB logging behavior.
3. If the schema exists, preserve the existing duplicate, `IF NOT EXISTS`, and
   `OR REPLACE` catalog-backed behavior.
4. If a runtime schema directory exists, leave MariaDB's original directory path
   untouched so any still-active legacy runtime state keeps its existing
   behavior.

## Affected Subsystems

- `mariadb/sql/sql_db.cc` database-create flow.
- MyLite SQL schema hook registry and storage-backed schema hook callbacks.
- `packages/libmylite/tests/embedded_storage_engine_test.c` storage-smoke
  schema lifecycle coverage.

## DDL Metadata Routing Impact

Initial schema DDL becomes catalog-owned for file-backed MyLite connections.
Table DDL remains routed through the MyLite handler and must work without a
schema directory once schema existence is available through
`check_db_dir_existence()` and catalog table discovery.

## Single-File And Lifecycle Impact

Durable schema metadata stays in the primary `.mylite` file from the first
create. The slice removes the last planned runtime schema directory for
file-backed initial schema creation. The MariaDB runtime datadir still exists
for embedded bootstrap, system state, and temporary runtime-owned work.

## Public API And File-Format Impact

No public API or storage format change is expected. Existing schema catalog
records hold the necessary schema name and option data.

## Storage-Engine Routing Impact

Immediate table DDL after directory-free schema creation must still route
omitted/default and `ENGINE=InnoDB` requests to effective `MYLITE` storage.

## Binary-Size, License, And Dependency Impact

No new dependency or license change is expected. Binary-size impact is limited
to a small MariaDB SQL-layer branch and additional test code.

## Test Plan

1. Open a file-backed MyLite database and create a schema with explicit charset,
   collation, and comment options.
2. Assert the schema exists in MyLite storage and that no runtime schema
   directory exists immediately after `CREATE DATABASE`.
3. Assert `SHOW DATABASES`, `SHOW CREATE DATABASE`, and
   `INFORMATION_SCHEMA.SCHEMATA` see the schema and options.
4. `USE` the schema, create an `ENGINE=InnoDB` table, insert and read a row, and
   assert requested/effective engine metadata.
5. Close/reopen, assert no runtime schema directory exists, then prove `USE`,
   metadata, table discovery, and row reads still work.
6. Cover `CREATE SCHEMA` alias behavior on a second catalog-only schema.
7. Preserve the existing `:memory:` smoke so hook activation does not affect
   in-memory behavior.
8. Run focused storage-smoke tests, sidecar/routed DDL harness reports,
   formatting, shell checks, clang-tidy, and full `dev`, `embedded-dev`, and
   `storage-smoke-dev` gates.

## Acceptance Criteria

- File-backed initial schema creation creates no runtime schema directory.
- Schema options are visible before and after close/reopen.
- Table DDL and DML work immediately under the catalog-only schema.
- Existing duplicate, `IF NOT EXISTS`, and `OR REPLACE` create behavior remains
  intact.
- Durable sidecar gates pass.

## Implementation Status

Implemented:

- `mysql_create_db_internal()` routes file-backed MyLite schema creates through
  the schema hook path whenever the runtime schema directory is absent.
- Missing schemas store catalog options directly and report the same affected
  row behavior as MariaDB's missing-directory create path.
- Existing catalog-only schemas keep the duplicate, `IF NOT EXISTS`, and
  `OR REPLACE` behavior from the earlier existence-options slice.
- Storage-smoke coverage verifies directory-free initial `CREATE DATABASE`,
  `CREATE SCHEMA`, missing-schema `IF NOT EXISTS`, missing-schema `OR REPLACE`,
  immediate `ENGINE=InnoDB` table DDL/DML, schema options, close/reopen
  discovery, and sidecar gates.

Measured rebuilt MariaDB embedded archive sizes:

| Build | Size |
| --- | ---: |
| `tools/mariadb-embedded-build all` | 30.57 MiB |
| `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC` | 30.71 MiB |

## Risks And Unresolved Questions

- Some future object types may still assume schema directories; this slice
  covers schemas and routed MyLite tables only.
- SQL-layer schema DDL rollback remains bounded by the current statement
  checkpoint design rather than full transactional DDL.

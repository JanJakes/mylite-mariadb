# SQL-Layer Schema Hooks

## Problem

MyLite persists schema names and options in the `.mylite` catalog, but the
MariaDB SQL layer still treats databases as directories under the runtime
datadir. `libmylite` currently recreates transient directories from the catalog
on file-backed open so `USE`, `SHOW DATABASES`, table listing, and
`INFORMATION_SCHEMA.SCHEMATA` continue to work.

That bridge is acceptable as temporary MyLite-owned runtime state, but it is not
the target architecture. Schema existence and discovery should flow from the
MyLite catalog when the static MyLite storage engine owns the primary file.

## Source Findings

Base authority: MariaDB 11.8.6, initial import ref
`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.

- `mariadb/sql/sql_db.cc` implements `mysql_create_db_internal()`,
  `mysql_alter_db_internal()`, `mysql_rm_db_internal()`,
  `load_db_opt_by_name()`, and `check_db_dir_existence()` around datadir
  directories and `db.opt`.
- `mariadb/sql/sql_show.cc` builds database lists in `make_db_list()` through
  `find_files(..., mysql_data_home, ...)`, validates
  `INFORMATION_SCHEMA.SCHEMATA` lookups with
  `verify_database_directory_exists()`, and builds table lists through
  `make_table_name_list()`.
- `mariadb/sql/sql_show.cc::find_files()` requires a readable database
  directory before it asks storage engines for discovered table names.
- `mariadb/sql/sql_show.cc::mysqld_show_create_db()` rejects missing database
  directories before it loads schema options through `load_db_opt_by_name()`.
- `mariadb/storage/mylite/ha_mylite.cc` already owns the active
  `--mylite-primary-file` value and exposes table discovery through handler
  callbacks backed by MyLite storage catalog APIs.
- `packages/libmylite/src/database.cc` currently calls
  `rehydrate_schema_directories()` after configuring a file-backed MyLite
  connection.

## Design

- Add a narrow MariaDB SQL-layer hook registry in `mariadb/sql/`.
- Register the hooks from the static MyLite handler when the plugin is
  initialized, and make them active only when `--mylite-primary-file` is set.
- Route these SQL-layer operations to MyLite storage when hooks are active and
  no live runtime schema directory is available:
  - schema existence checks,
  - schema alteration and drop publication,
  - schema option reads for `db.opt` callers,
  - database listing for `SHOW DATABASES` and `INFORMATION_SCHEMA.SCHEMATA`,
  - table listing when the database directory is absent.
- Stop `libmylite` from rehydrating schema directories on file-backed open.
- A later directory-free create slice routes file-backed initial
  `CREATE DATABASE` through the same catalog hooks once table DDL no longer
  needs that transient directory during the active connection.
- Keep the existing direct/prepared schema sync path as a conservative
  compatibility belt until broader raw MariaDB adapter paths exist.

## Affected MariaDB Subsystems

- `mariadb/sql/sql_db.cc` database DDL, option loading, and existence checks.
- `mariadb/sql/sql_show.cc` database and table discovery.
- `mariadb/storage/mylite/ha_mylite.cc` hook registration and catalog-backed
  hook implementations.
- `packages/libmylite/src/database.cc` embedded connection setup.

## Compatibility Impact

Covered in this slice:

- Reopen no longer recreates transient schema directories before user SQL.
- `USE`, `SHOW DATABASES`, `SHOW TABLES`, `SHOW CREATE DATABASE`,
  `INFORMATION_SCHEMA.SCHEMATA`, and table resolution can use catalog-backed
  schema state when no schema directory exists.
- No-directory `ALTER DATABASE` and `DROP DATABASE` publish through the catalog
  hooks.

Still planned after this slice:

- Fully filesystem-free object paths for views, triggers, routines, and other
  non-table objects.
- SQL transaction rollback semantics for schema DDL.
- Raw MariaDB C API adapter coverage outside the primary `libmylite` API.

## DDL Metadata Routing Impact

Schema DDL starts publishing through the same MyLite catalog APIs used by
`libmylite` schema sync. Table DDL remains routed through the MyLite handler.

## Single-File And Embedded Lifecycle

Durable schema state remains in the primary `.mylite` file. MyLite no longer
creates transient schema directories during file-backed open. A later
directory-free create slice extends the same catalog-backed path to initial
file-backed schema creation. The runtime datadir still exists for MariaDB
bootstrap state and server-owned system schemas.

## Public API And File Format

The public `libmylite` C API and MyLite storage file format do not change. The
new hook layer is an internal MariaDB fork interface between SQL core and the
static MyLite handler.

## Storage-Engine Routing Impact

The hook layer is active only when the MyLite handler has a primary file.
`:memory:` sessions and builds without the MyLite storage engine retain the
existing MariaDB directory behavior.

## Wire Protocol Or Integration Impact

Future wire-protocol integrations that use the same embedded MyLite runtime
should inherit the catalog-backed schema behavior.

## Binary Size Impact

Expected size impact is small: one SQL hook registry and handler glue. Record
the measured size table after implementation because this touches MariaDB
sources and the storage-smoke archive.

## License Or Dependency Impact

No dependency is introduced. Changes are GPL-2.0-compatible MariaDB fork code.

## Test And Verification Plan

- Extend storage-engine smoke tests to assert reopen does not rehydrate runtime
  schema directories.
- Cover reopen `USE`, `SHOW DATABASES`, `SHOW TABLES`, no-directory
  `ALTER DATABASE`, `SHOW CREATE DATABASE`, and `INFORMATION_SCHEMA.SCHEMATA`
  without rehydrated schema directories.
- Preserve sidecar lifecycle checks after close/reopen.
- Run formatting, tidy, dev, embedded, storage-smoke, compatibility report, size
  report, and `git diff --check`.

## Acceptance Criteria

- Reopen does not need `CREATE DATABASE IF NOT EXISTS` rehydration.
- Reopened file-backed MyLite databases can use, list, alter, show, and drop
  catalog-backed schemas without runtime schema directories.
- Existing routed table discovery and row/index smoke coverage still passes.
- Documentation explicitly narrows the remaining database-object gap to
  non-table objects and transactional DDL semantics.

## Risks And Unresolved Questions

- MariaDB still has many object-specific filesystem paths. This slice covers the
  schema and table-discovery layer, not views, triggers, routines, or events.
- This slice originally left initial `CREATE DATABASE` on the transient
  runtime directory path; `docs/specs/directory-free-create-database/specs.md`
  covers the follow-up removal for file-backed schema creation.
- `DROP DATABASE` affected-row counts may need a later compatibility pass once
  broader object metadata is catalog-backed.
- The hook registry is a fork-local interface; it should stay narrow to avoid
  making future upstream rebases harder.

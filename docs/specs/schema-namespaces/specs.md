# Schema Namespaces

## Problem

MyLite currently stores table definitions with a schema name, but the embedded
MariaDB runtime still treats schemas as directories under a temporary datadir.
After close/reopen the runtime directory is new, so tests recreate
`CREATE DATABASE app` before `USE app`. That keeps table discovery working, but
it makes schema existence depend on a transient directory rather than the
`.mylite` catalog.

The slice promotes schema names to catalog-backed namespaces and rehydrates the
temporary MariaDB runtime from that catalog when a file-backed MyLite database
opens.

## Source Findings

Base authority: MariaDB 11.8.6, initial import ref
`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.

- `mariadb/sql/sql_db.cc` implements `mysql_create_db_internal()`,
  `mysql_rm_db_internal()`, and `mysql_change_db()` around filesystem
  directories and `db.opt` files. `mysql_change_db()` calls
  `check_db_dir_existence()` before accepting `USE`.
- `mariadb/sql/sql_show.cc` implements `SHOW DATABASES` and
  `INFORMATION_SCHEMA.SCHEMATA` through `make_db_list()` and `find_files()`,
  which enumerate directories below `mysql_data_home`.
- `mariadb/sql/handler.cc` exposes table discovery callbacks through
  `ha_discover_table_names()` and `ha_table_exists()`, but there is no matching
  storage-engine callback for database creation or database-name discovery.
- `mariadb/storage/mylite/ha_mylite.cc` already implements
  `discover_table()`, `discover_table_names()`, and
  `discover_table_existence()` from MyLite catalog table records.
- `packages/libmylite/src/database.cc` starts MariaDB with a fresh MyLite-owned
  temporary `--datadir`, passes `--mylite-primary-file=<file>` for storage
  smoke builds, and removes the runtime directory on final close.

## Design

- Add first-party MyLite storage APIs for schema records:
  `mylite_storage_store_schema()`, `mylite_storage_drop_schema()`,
  `mylite_storage_schema_exists()`, and `mylite_storage_list_schemas()`.
- Extend catalog record validation with a schema record type. Existing table
  records remain valid, and schema listing also treats table-record schema names
  as namespaces so older files with only table records can be rehydrated.
- Keep durable schema state in the `.mylite` file. MariaDB runtime directories
  created by `CREATE DATABASE` remain temporary compatibility state and are
  deleted with the MyLite runtime directory.
- On file-backed open, list catalog schema names and execute internal
  `CREATE DATABASE IF NOT EXISTS` statements to recreate only transient runtime
  directories before user SQL runs.
- After successful direct `CREATE/DROP DATABASE` or `CREATE/DROP SCHEMA`, sync
  runtime database names back to the MyLite catalog. Prepared DDL schema sync is
  not included in this slice.

## Affected Subsystems

- MyLite storage catalog format and APIs.
- `libmylite` embedded runtime startup and direct execution path.
- Storage-engine smoke tests that currently recreate the schema after reopen.
- Compatibility matrix and storage architecture documentation.

## Compatibility Impact

Covered in this slice:

- `CREATE DATABASE` / `CREATE SCHEMA` persist the namespace after successful
  direct execution.
- `USE <schema>` works after close/reopen without rerunning `CREATE DATABASE`.
- `SHOW DATABASES` and `INFORMATION_SCHEMA.SCHEMATA` see rehydrated catalog
  namespaces through MariaDB's existing directory-based enumeration.
- `DROP DATABASE` / `DROP SCHEMA` remove empty schema catalog metadata and
  table metadata already dropped through MariaDB's table-drop path.

Still planned:

- Persistent schema options such as default character set, collation, and
  comments.
- Prepared-statement DDL schema sync.
- SQL-layer database hooks that avoid the temporary directory bridge entirely.
- Views, triggers, routines, and privilege/account semantics.

## DDL Metadata Routing Impact

Table metadata remains routed through the MyLite handler. Schema metadata is
stored by the first-party storage API and synchronized by `libmylite` after
successful direct database DDL. Table records continue to carry schema names,
and schema listing deduplicates explicit schema records with table-backed
schemas.

## Single-File And Embedded Lifecycle

Durable namespace state lives in the primary `.mylite` file. Runtime
directories are MyLite-owned transient companions under the temporary runtime
directory and are removed on final close. Reopen reconstructs the transient
directory names from the catalog before user SQL runs.

## Public API And File Format

The public `libmylite` C API does not change. The internal storage API gains
schema catalog functions and a schema capability flag. The catalog gains a new
record type while keeping the current file-format version during early
development.

## Storage-Engine Routing

This slice does not change table engine routing. It makes schema existence
independent of the requested table engine so `ENGINE=InnoDB`, `ENGINE=MyISAM`,
`ENGINE=Aria`, omitted engine, and `ENGINE=MYLITE` tables all reopen under
catalog-backed schemas.

## Binary Size

Expected size impact is small: one catalog record type, schema-list helpers,
and embedded runtime sync code. Size reporting remains part of verification.

## Test Plan

- Add storage unit coverage for storing, listing, checking, and dropping schema
  records, including table-backed schema discovery.
- Extend storage-engine smoke coverage so a file is reopened and `USE app`
  succeeds without rerunning `CREATE DATABASE app`.
- Cover an empty schema that survives close/reopen, appears in `SHOW DATABASES`,
  has no tables, and disappears after `DROP DATABASE`.
- Run storage, embedded, storage-smoke, formatting, tidy, compatibility report,
  and size checks.

## Acceptance Criteria

- Catalog schema APIs are validated and covered by unit tests.
- File-backed MyLite reopen reconstructs schema runtime directories from the
  catalog.
- Existing table discovery and row/index tests pass without recreating the
  schema after reopen in at least one representative storage-engine smoke path.
- Documentation marks schemas/databases as partial with explicit remaining
  limits.

## Risks And Unresolved Questions

- The temporary directory bridge is not the final SQL-layer solution. It is
  acceptable only because the directories are transient MyLite-owned runtime
  state, not durable application storage.
- Schema option persistence needs a catalog structure beyond a schema name.
- Prepared DDL and raw MariaDB C API adapter paths will need their own sync or
  SQL-layer hook before they can claim schema DDL coverage.

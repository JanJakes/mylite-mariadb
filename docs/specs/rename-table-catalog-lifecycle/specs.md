# RENAME TABLE Catalog Lifecycle

## Goal

Implement `RENAME TABLE` for MyLite-routed base tables by updating the table's
catalog identity inside the primary `.mylite` file. Renamed tables must remain
backed by the same table id so existing keyless row pages move with the logical
table name. The implementation must not create, rename, or depend on MariaDB
durable sidecar files.

## Non-Goals

- Do not implement `ALTER TABLE`, truncate, update, delete-row,
  `ALTER TABLE ... RENAME INDEX`, high-level index rename handling, or
  free-space reclamation.
- Do not rewrite table-definition blob pages unless tests prove MariaDB needs
  the stored definition image changed for simple renames.
- Do not add transactional DDL, crash recovery, or concurrent writer support.
- Do not add `.frm` rename as a compatibility fallback.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_table.cc:5544-5611` builds extensionless source and target
  table paths, creates an engine handler, and calls
  `handler::ha_rename_table()` when `QRMT_HANDLER` is set.
- `mariadb/sql/handler.cc:5885-5900` implements `ha_rename_table()` as a
  transaction-marking wrapper around `handler::rename_table()`.
- `mariadb/sql/handler.h:5139-5145` documents that engines may override
  `rename_table()` instead of using the default extension-based file rename.
- `mariadb/sql/handler.cc:5382-5405` shows the default implementation renames
  every file returned by `bas_ext()`, which is not a valid MyLite final storage
  model.
- `mariadb/sql/sql_table.cc:5611-5650` may rename high-level indexes after the
  base table rename. MyLite treats ordinary table renames as catalog identity
  updates; SQL-level index rename behavior remains explicit separate work.
- `mariadb/sql/sql_table.cc:5674-5691` reports handler rename failures as
  `ER_ERROR_ON_RENAME` except `HA_ERR_WRONG_COMMAND`, which becomes a generic
  "ALTER TABLE" not-supported message in this path.

The current MyLite handler returns `HA_ERR_WRONG_COMMAND` from `rename_table()`,
so `RENAME TABLE` is deliberately unsupported. After row pages exist, rename
must preserve the catalog table id; delete-plus-create would orphan live rows
or risk exposing the wrong row set.

## Compatibility Impact

`RENAME TABLE` becomes partial support for simple MyLite-routed base-table
renames. It updates table discovery, `SHOW TABLES`, and direct `SELECT` access
so the old name disappears and the new name sees the same rows and supported
indexes. Cross-schema renames are catalog namespace updates when MariaDB accepts
the statement.

Existing target names must fail rather than overwrite catalog records.
Unsupported explicit engines still fail before catalog publication.

## Design

Add a MyLite storage API that renames one table-definition catalog record:

- validate the header and catalog root;
- find the source schema/table record;
- reject the rename if the target schema/table already exists;
- rebuild the catalog root page, replacing only the source record's schema and
  table fields;
- preserve table id, definition root page, definition size, requested engine,
  and effective engine;
- increment the catalog generation and write the catalog page plus header.

The physical table-definition blob pages and row pages stay in place. Keeping
the table id stable is what makes existing row pages visible through the new
logical name.

The handler `rename_table()` should parse source and target schema/table names
from MariaDB's canonical table paths and call the storage rename API. It should
return MariaDB handler errors through the existing storage-to-handler mapping.

## DDL Metadata Routing Impact

This slice completes the first catalog rename path for routed base tables.
`CREATE TABLE`, discovery, `DROP TABLE`, and `RENAME TABLE` become
catalog-backed for simple routed tables.

`ALTER TABLE` remains unsupported because copy/rebuild and in-place algorithms
need explicit decisions for row/index storage, table-definition images,
transactional DDL, and recovery.

## Storage-Engine Routing Impact

All currently accepted routed engines use the same MyLite rename path:

- omitted/default engine,
- `ENGINE=MYLITE`,
- `ENGINE=InnoDB`,
- `ENGINE=MyISAM`,
- `ENGINE=Aria`.

The rename operation updates MyLite catalog identity only; it does not attempt
to invoke or emulate the requested engine's external file lifecycle.

## File Lifecycle

`RENAME TABLE` changes only the primary `.mylite` file. No persistent `.frm`,
`.ibd`, `.MYD`, `.MYI`, `.MAI`, `.MAD`, `aria_log.*`, redo, undo, binlog,
relay-log, or plugin-owned table files should appear or be renamed.

This slice does not add journals or WAL. A crash during catalog rewrite is
still governed by the current non-transactional catalog risk until the recovery
slice.

## Embedded Lifecycle And API

No public `libmylite` API changes are required. `mylite_exec()` should report
successful `RENAME TABLE` through existing direct execution behavior. After
close and reopen, renamed tables must remain visible only at the new catalog
identity.

## Build, Size, And Dependencies

No new dependency is introduced. The storage-smoke static handler build grows by
first-party catalog rewrite and path parsing calls only. The default embedded
baseline remains unchanged.

## Test Plan

1. Add storage unit coverage for renaming a table definition in the catalog.
2. Add storage unit coverage proving the table id is preserved by checking that
   existing keyless row pages are visible through the new name and absent from
   the old name.
3. Add storage unit coverage for missing source and duplicate target failures.
4. Extend storage-engine smoke coverage so `RENAME TABLE` succeeds for a
   keyless MyLite-routed table, updates catalog discovery, preserves rows, and
   leaves no known durable sidecars.
5. Run `dev`, `storage-smoke-dev`, `embedded-dev`, format checks, clang-tidy,
   and `git diff --check`.

## Acceptance Criteria

- `RENAME TABLE` succeeds for simple MyLite-routed tables.
- The old table name disappears from catalog existence checks, `SHOW TABLES`,
  and reopen discovery.
- The new table name discovers the same table definition and existing keyless
  rows.
- Existing target names fail without changing either table.
- Table id is preserved across rename.
- Docs and compatibility matrices describe the partial DDL lifecycle accurately.

## Implementation Status

Implemented in the storage package and MyLite handler:

- `mylite_storage_rename_table()` rebuilds the catalog root with a renamed
  table-definition record, rejects missing sources and duplicate targets, and
  increments the catalog generation.
- Rename preserves table id, definition blob reference, requested engine, and
  effective engine metadata so existing keyless row pages are visible through
  the new table name.
- `ha_mylite::rename_table()` parses MariaDB's canonical source and target
  paths and routes accepted engine requests through the MyLite catalog rename
  path.
- Storage unit coverage checks row visibility through the renamed table,
  old-name absence, duplicate target failure, missing source failure, preserved
  metadata, and list-table updates.
- Storage-engine smoke coverage checks SQL `RENAME TABLE`, row preservation,
  catalog absence at old names, supported indexed lookups and duplicate checks,
  close/reopen discovery, and no durable sidecars.

## Risks And Open Questions

- Catalog rewrite is still non-transactional; crash-safe DDL belongs to the
  recovery slice.
- MariaDB's stored table-definition image may contain edge-case identity data
  for quoted identifiers or unusual object types; smoke tests will validate the
  simple base-table path.
- SQL-level index renames remain separate from preserving existing indexes
  across a base-table rename.

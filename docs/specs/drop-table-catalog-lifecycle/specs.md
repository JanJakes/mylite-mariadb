# DROP TABLE Catalog Lifecycle

## Goal

Implement `DROP TABLE` for MyLite-routed tables by removing the table from the
MyLite catalog without creating or deleting MariaDB durable sidecars. Dropped
tables must stop being discoverable immediately, and recreating the same
schema/table name must not expose row pages that belonged to the old table.

## Non-Goals

- Do not implement `RENAME TABLE`, `ALTER TABLE`, truncate, update, delete-row,
  indexes, or free-space reclamation.
- Do not physically rewrite or zero old table-definition blob pages or row
  pages in this slice.
- Do not add transactional DDL, crash recovery, or concurrent writer support.
- Do not add MariaDB datadir files or `.frm` deletion as a compatibility
  fallback.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_table.cc:1685-1710` calls `ha_delete_table()` during
  `DROP TABLE`; engines with table discovery suppress ordinary missing-file
  warnings.
- `mariadb/sql/handler.cc:3353-3369` routes `ha_delete_table()` through the
  handlerton `drop_table` hook.
- `mariadb/sql/handler.cc:567-582` implements the default handlerton
  `drop_table` hook by creating a handler and calling `handler::delete_table()`
  with the canonical table path.
- `mariadb/sql/handler.h:5139-5153` documents that engines can override
  `delete_table()` instead of relying on extension-based file deletion.
- `mariadb/sql/handler.cc:5350-5365` shows the default `delete_table()` deletes
  files returned by `bas_ext()`, which is not a valid MyLite final storage
  model.
- `mariadb/storage/example/ha_example.cc:804-825` notes that all open
  references are closed before `delete_table()` and that the `name` argument is
  the table name/path the engine must remove.

The current MyLite handler returns `HA_ERR_WRONG_COMMAND` from `delete_table()`,
so `DROP TABLE` is deliberately unsupported. After row pages exist, dropping
only the catalog entry is not enough unless new table ids avoid orphaned row
pages from previous incarnations of the same name.

## Compatibility Impact

`DROP TABLE` becomes partial support for MyLite-routed base tables. It removes
the table from `SHOW TABLES`, table discovery, and direct `SELECT` access.
Dropping a table with persisted keyless rows leaves unreachable row pages in the
primary file until free-space management exists. Recreating the same name must
produce an empty logical table.

`DROP TABLE IF EXISTS` should continue to behave through MariaDB's normal drop
path; the storage layer reports missing catalog records as not found.

## Design

Add a MyLite storage API that removes one table-definition record from the
catalog root page:

- validate the header and catalog root;
- find the matching schema/table record;
- rebuild the catalog root page without that record;
- increment the catalog generation and write the catalog page plus header.

The physical table-definition blob pages and row pages stay in the file for
now. They are unreachable because discovery starts from the catalog.

Update table id allocation for new table-definition records. The current
record-count-based id is not safe after deletion. New creates must allocate
`max(existing catalog table ids, existing row-page table ids) + 1`. That keeps
orphaned row pages invisible after drop/recreate even before page reclamation
exists.

The handler `delete_table()` should parse the schema and table name from
MariaDB's canonical table path and call the storage drop API. It should return
MariaDB handler errors through the existing storage-to-handler mapping.

## DDL Metadata Routing Impact

This slice completes the first catalog-removal path for routed base tables.
`CREATE TABLE` and discovery remain catalog-backed. `DROP TABLE` updates only
MyLite catalog metadata, not external table files.

`RENAME TABLE` and `ALTER TABLE` remain unsupported because they require
definition-image updates, row/index metadata decisions, and transactional DDL
semantics.

## Storage-Engine Routing Impact

All currently accepted routed engines use the same MyLite drop path:

- omitted/default engine,
- `ENGINE=MYLITE`,
- `ENGINE=InnoDB`,
- `ENGINE=MyISAM`,
- `ENGINE=Aria`.

Unsupported explicit engines still fail before catalog publication and
therefore have nothing for MyLite to drop.

## File Lifecycle

`DROP TABLE` changes only the primary `.mylite` file. No persistent `.frm`,
`.ibd`, `.MYD`, `.MYI`, `.MAI`, `.MAD`, `aria_log.*`, redo, undo, binlog,
relay-log, or plugin-owned table files should appear. Orphaned pages are
internal free-space debt inside the primary file, not external durable assets.

This slice does not reclaim pages and does not add journals or WAL. A crash
during catalog rewrite is still governed by the current non-transactional
catalog risk until the recovery slice.

## Embedded Lifecycle And API

No public `libmylite` API changes are required. `mylite_exec()` should report
successful `DROP TABLE` through existing direct execution behavior. After close
and reopen, dropped tables must remain absent and recreated tables must not
inherit old rows.

## Build, Size, And Dependencies

No new dependency is introduced. The storage-smoke static handler build grows by
first-party catalog rewrite and path parsing code only. The default embedded
baseline remains unchanged.

## Test Plan

1. Add storage unit coverage for dropping a table definition from the catalog.
2. Add storage unit coverage proving drop/recreate does not reuse orphaned row
   pages from the dropped table.
3. Extend storage-engine smoke coverage so `DROP TABLE` succeeds for MyLite
   routed tables, removes catalog discovery, and leaves no known durable
   sidecars.
4. Keep `RENAME TABLE` unsupported until a later slice.
5. Run `dev`, `storage-smoke-dev`, `embedded-dev`, format checks, clang-tidy,
   and `git diff --check`.

## Acceptance Criteria

- `DROP TABLE` succeeds for MyLite-routed tables.
- Dropped tables disappear from catalog existence checks, `SHOW TABLES`, and
  reopen discovery.
- Drop/recreate of a keyless table yields an empty table, not old orphaned rows.
- Table id allocation is monotonic across catalog records and row pages.
- `RENAME TABLE` remains explicitly unsupported.
- Docs and compatibility matrices describe the partial DDL lifecycle accurately.

## Implementation Status

Implemented in the storage package and MyLite handler:

- `mylite_storage_drop_table()` removes a table-definition catalog record,
  increments the catalog generation, and rewrites the catalog root plus header.
- New table-definition ids are allocated above both live catalog records and
  persisted row-page table ids to keep dropped rows unreachable after
  drop/recreate.
- `ha_mylite::delete_table()` parses MariaDB's canonical table path and routes
  all accepted engine requests through the MyLite catalog drop path.
- Storage unit coverage checks drop, missing-table behavior, list-table
  updates, and drop/recreate with orphaned row pages.
- Storage-engine smoke coverage checks `DROP TABLE`, catalog absence, no
  durable sidecars, reopen behavior, and continued `RENAME TABLE` rejection.

## Risks And Open Questions

- Catalog rewrite is still non-transactional; crash-safe DDL belongs to the
  recovery slice.
- Orphaned definition and row pages grow the file until free-space management is
  implemented.
- Parsing schema/table names from MariaDB's canonical handler path may need more
  coverage for quoted identifiers, temporary names, and platform path variants.

# SHOW CREATE Round Trip

## Problem

MyLite already covers representative CHECK/generated dump-style import, but
export coverage is still mostly implicit. A real dump/export flow depends on
MariaDB reconstructing table DDL from catalog-backed metadata after reopen, and
on that DDL being importable through MyLite without durable sidecars.

This slice adds a representative `SHOW CREATE TABLE` round trip for a routed
table that combines autoincrement state, generated columns, CHECK constraints,
supported indexes, and requested-engine routing.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_show.cc:1196-1301` implements `SHOW CREATE TABLE` field
  setup and calls `show_create_table()` for the opened table.
- `mariadb/sql/sql_show.cc:1988-2040` calls the handler's
  `update_create_info()` before printing table options, then emits
  `AUTO_INCREMENT=N` when the handler reports a next value greater than one.
- `mariadb/sql/sql_show.cc:2159-2404` reconstructs table SQL from the MariaDB
  table share, including engine name, columns, generated-column expressions,
  keys, and column attributes.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::update_create_info()`
  reports MyLite's durable autoincrement state for copy ALTER and SHOW paths.
- MyLite catalog-backed reopen already restores the packed MariaDB table
  definition image, so `SHOW CREATE TABLE` should not require durable `.frm`
  sidecars.

## Design

Add storage-engine smoke coverage that:

- creates a routed `ENGINE=InnoDB` table with:
  - a single-column autoincrement primary key,
  - generated columns,
  - CHECK constraints,
  - supported unique and secondary indexes,
  - a bounded BLOB/TEXT prefix index,
  - representative row data that advances the autoincrement counter;
- closes and reopens the database so `SHOW CREATE TABLE` is served from
  catalog-backed metadata rather than active runtime schema directories;
- captures the `SHOW CREATE TABLE` output;
- imports that DDL into another schema; and
- verifies inserted rows, generated values, CHECK enforcement, duplicate-key
  enforcement, indexed reads, requested-engine metadata, autoincrement next
  value, close/reopen visibility, and sidecar absence.

## Supported Scope

- Representative `SHOW CREATE TABLE` round trip for one routed table shape.
- Catalog-backed export after close/reopen.
- Reimport into another schema through MyLite's normal routed DDL path.

## Non-Goals

- A full `mariadb-dump` CLI integration.
- Exhaustive dump/export option matrices.
- View, trigger, routine, partition, FULLTEXT, SPATIAL, or unsupported-index
  export. Foreign-key parent/child export/import is covered by the separate
  `show-create-foreign-key-round-trip` slice.
- SQL-mode-sensitive formatting comparisons beyond the embedded default SQL
  mode used by current smoke tests.

## Compatibility Impact

The compatibility matrix can claim representative `SHOW CREATE TABLE`
round-trip export/import coverage for routed table metadata. Broader
dump/export compatibility remains planned.

## DDL Metadata Routing Impact

The source table is reopened from MyLite catalog metadata before export. The
imported table follows the normal routed `CREATE TABLE` handler path and
publishes a second catalog table-definition record in the target schema.

## Single-File And Embedded-Lifecycle Impact

No new file format or companion files are introduced. The test proves exported
DDL can be produced after final close and reopen without persistent MariaDB
sidecars.

## Public API And File-Format Impact

No public `libmylite` API or storage file-format change is required.

## Storage-Engine Routing Impact

The source and imported tables request `ENGINE=InnoDB`, which resolves to the
MyLite handler. The imported catalog metadata must preserve requested engine
`InnoDB` and effective engine `MYLITE`.

## Binary-Size And Dependency Impact

No dependency or binary-size-sensitive runtime code is added.

## Test And Verification Plan

- Add storage-engine smoke coverage for the round trip.
- Update compatibility, roadmap, storage architecture, and dump/import-related
  specs to distinguish representative `SHOW CREATE` round-trip coverage from
  broader dump/export support.
- Run format, targeted storage-smoke tests, harness reports for
  `routed-ddl-dml`, `check-constraint`, `generated-column`,
  `application-schema`, and full preset gates.

## Acceptance Criteria

- `SHOW CREATE TABLE` after close/reopen returns importable DDL for the
  representative routed table.
- The imported table preserves requested-engine metadata.
- Generated columns, CHECK constraints, supported indexes, BLOB/TEXT prefix
  indexes, and autoincrement state work after import and reopen.
- Docs keep broader dump/export coverage marked as planned.

## Implementation Status

Implemented in the MyLite handler and storage-engine smoke:

- Opened catalog-backed tables expose the requested compatible engine name to
  MariaDB `SHOW CREATE TABLE` while continuing to execute through the MyLite
  handler.
- Storage-engine smoke exports representative routed `ENGINE=InnoDB` DDL after
  close/reopen, imports it into another schema, and verifies generated columns,
  CHECK constraints, supported indexes, bounded BLOB/TEXT prefix indexes,
  requested-engine metadata, autoincrement next value, close/reopen visibility,
  and sidecar absence.
- Foreign-key parent/child `SHOW CREATE TABLE` export/import is covered by the
  separate `show-create-foreign-key-round-trip` slice.

## Risks And Unresolved Questions

- `SHOW CREATE TABLE` formatting is MariaDB-owned and may change with the
  selected upstream baseline; the test should validate behavior, not depend on
  unnecessary formatting minutiae.
- Real dump tooling may include session state, comments, locks, temporary
  table handling, or object classes that remain outside this slice.

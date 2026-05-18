# Altered Table Dump Roundtrip

## Goal

Broaden dump/export coverage for routed tables whose final definition is
created through supported copy `ALTER` operations rather than only through
initial `CREATE TABLE` syntax. MyLite already covers representative dump-style
CHECK/generated imports and initial-definition `SHOW CREATE TABLE` round trips;
this slice proves an ALTER-evolved table exports a reusable definition.

## Non-Goals

- Do not add a MyLite dump exporter.
- Do not import `mysqldump` lock, key-disabling, or server-session wrappers.
- Do not add durable transactional DDL semantics.
- Do not change the MyLite public API, file format, or size profile.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc:mysql_alter_table()` and
  `copy_data_between_tables()` rebuild supported copy-ALTER tables and publish
  the resulting table definition.
- `mariadb/sql/sql_show.cc:show_create_table_ex()` formats `SHOW CREATE TABLE`
  output from the reopened `TABLE_SHARE`.
- `mariadb/sql/unireg.cc` packs CHECK and generated-column metadata into the
  table-definition image.
- `mariadb/sql/table.cc:parse_vcol_defs()` and
  `TABLE::update_virtual_fields()` restore and compute generated columns from
  the stored definition.
- `mariadb/storage/mylite/ha_mylite.cc` stores MariaDB table-definition images
  and retained key metadata in the primary `.mylite` file during create and
  copy-ALTER publication.

## Compatibility Impact

This narrows the broader dump/export gap for MyLite-routed tables. The covered
shape mirrors a common migration flow: create a simple table, add generated
columns, indexes, and CHECK constraints through ALTER, then export the resulting
definition for import elsewhere.

The slice does not claim complete `mysqldump` compatibility. Server-locking,
session-wrapper, and key-disabling statements remain governed by the existing
server-surface policy.

## Design

Add storage-engine smoke coverage that:

1. creates a routed `ENGINE=InnoDB` table with an autoincrement primary key,
2. inserts source rows,
3. adds stored and virtual generated columns through copy ALTER,
4. adds generated, virtual, and BLOB/TEXT prefix indexes through copy ALTER,
5. adds named CHECK constraints through copy ALTER,
6. closes and reopens to force catalog-backed discovery,
7. captures `SHOW CREATE TABLE`,
8. imports that definition into a fresh schema,
9. verifies autoincrement, generated values, forced-index reads, CHECK
   enforcement, and unique-key enforcement, and
10. closes and reopens the imported schema to prove durable metadata.

## File Lifecycle

All source and imported definitions, rows, generated metadata, CHECK metadata,
and index entries must live in the primary `.mylite` file. The test retains the
existing durable-sidecar gate.

## Embedded Lifecycle And API

No public API change is required. The behavior is visible through ordinary
direct SQL execution and `SHOW CREATE TABLE`.

## Storage-Engine Routing

The table uses explicit `ENGINE=InnoDB`, which routes to MyLite while
preserving the requested engine name in exported DDL.

## Build, Size, And Dependencies

No dependency or intended size-profile change is introduced.

## Test Plan

- Add storage-engine smoke coverage for ALTER-evolved table export/import.
- Update compatibility, storage architecture, roadmap, and generated-column
  coverage docs.
- Build `mylite_embedded_storage_engine_test`.
- Run the focused storage-engine smoke binary.
- Run `ctest --preset storage-smoke-dev`, `ctest --preset dev`, and
  `git diff --check`.

## Acceptance Criteria

- The ALTER-evolved table exports with generated columns, CHECK constraints,
  supported indexes, requested engine, and autoincrement state.
- The exported definition imports into a fresh schema without durable sidecars.
- Imported rows use the exported autoincrement value and generated metadata.
- Imported generated-column indexes, BLOB/TEXT prefix indexes, CHECK
  constraints, and generated unique constraints work before and after reopen.

## Risks And Unresolved Questions

- This remains representative coverage. Full dump/export compatibility still
  needs broader `mysqldump` grammar, session wrapper, and application fixture
  suites.

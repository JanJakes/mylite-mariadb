# SHOW CREATE Foreign-Key Round Trip

## Goal

Extend representative dump/export coverage to a parent/child foreign-key table
pair. `SHOW CREATE TABLE` must export FK metadata from catalog-backed MyLite
tables after close/reopen, and the exported DDL must import into a fresh schema
with FK row checks and supported actions intact.

## Non-Goals

- Do not add full `mariadb-dump` CLI compatibility.
- Do not cover views, triggers, routines, partitions, temporary tables, or
  unsupported FK/index shapes.
- Do not cover cyclic FK graphs or exhaustive action matrices.
- Do not add physical compaction for pages made unreachable by later drops.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_show.cc:1196-1301` implements the `SHOW CREATE TABLE`
  result shape and calls `show_create_table()` for the opened table.
- `mariadb/sql/sql_show.cc:2159-2404` reconstructs table SQL from the MariaDB
  table share and delegates FK text to storage-engine metadata hooks when the
  engine publishes FK information.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::get_foreign_key_list()`
  exposes MyLite-owned FK metadata to MariaDB information-schema and
  `SHOW CREATE TABLE` paths.
- `mariadb/storage/mylite/ha_mylite.cc:mylite_append_foreign_key_clause()`
  formats supported FK clauses, including referenced table/key names and
  supported actions.
- `packages/mylite-storage/src/storage.c:mylite_storage_list_foreign_keys()`
  reads child FK records from the primary `.mylite` catalog and decodes the
  blob-backed FK definition payload.

## Compatibility Impact

Representative `SHOW CREATE TABLE` export/import coverage now includes a
foreign-key parent/child pair with a unique referenced parent key and supported
`ON DELETE SET NULL` / `ON UPDATE CASCADE` actions. This narrows the broader
dump/export gap without claiming full dump-tool compatibility or exhaustive FK
action coverage.

## Design

Add storage-engine smoke coverage that:

1. Creates a routed InnoDB parent table with a primary key and unique referenced
   secondary key.
2. Creates a routed InnoDB child table with a supported FK referencing the
   unique parent key and supported action clauses.
3. Closes and reopens the database so export reads catalog-backed metadata.
4. Captures `SHOW CREATE TABLE` output for the parent and child.
5. Imports the parent and child DDL into a fresh schema.
6. Verifies requested-engine metadata, FK information-schema metadata,
   immediate child/parent checks, `ON UPDATE CASCADE`, `ON DELETE SET NULL`,
   close/reopen visibility, and durable sidecar absence.

No production change is expected if existing FK create-info and DDL publication
paths compose correctly.

## File Lifecycle

The export/import round trip stays inside the primary `.mylite` file plus
permitted transient MyLite recovery-journal state. No persistent `.frm`,
`.ibd`, `.MYD`, `.MYI`, `.MAI`, `.MAD`, Aria log, binlog, relay log, or
plugin-owned durable table file is introduced.

## Embedded Lifecycle And API

No public C API change is required. The behavior is exposed through
`mylite_exec()` and catalog-backed close/reopen discovery.

## Build, Size, And Dependencies

No dependency or size-profile change is intended. The test uses the existing
storage-smoke embedded build.

## Test Plan

- Add `libmylite` storage-engine smoke coverage for FK `SHOW CREATE TABLE`
  export/import after close/reopen.
- Assert the child export contains the FK clause and supported actions.
- Import the exported parent and child DDL into a different schema.
- Verify invalid child inserts remain protected.
- Verify supported update/delete actions mutate imported child rows correctly.
- Verify imported metadata and rows survive close/reopen without durable
  MariaDB sidecars.
- Run the focused storage-smoke test, storage-smoke CTest preset, default CTest
  preset, format check, and `git diff --check`.

## Acceptance Criteria

- Catalog-backed `SHOW CREATE TABLE` exports importable parent/child FK DDL.
- Imported FK metadata appears in information schema and `SHOW CREATE TABLE`.
- Imported child and parent row checks work.
- Imported `ON UPDATE CASCADE` and `ON DELETE SET NULL` actions work.
- Close/reopen keeps the imported FK metadata and rows visible.

## Implementation Status

Implemented in storage-engine smoke coverage:

- Catalog-backed parent and child tables export importable DDL after
  close/reopen.
- Child `SHOW CREATE TABLE` includes the FK clause, referenced unique parent
  key, `ON DELETE SET NULL`, and `ON UPDATE CASCADE`.
- The exported parent and child DDL import into a fresh schema with requested
  `InnoDB` metadata routed to MyLite.
- Imported FK metadata appears in information schema, rejects orphan child
  rows, cascades parent-key updates, applies parent-delete SET NULL, and
  survives close/reopen without durable MariaDB sidecars.

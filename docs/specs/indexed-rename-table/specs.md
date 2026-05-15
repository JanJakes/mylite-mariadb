# Indexed RENAME TABLE Coverage

## Goal

Extend `RENAME TABLE` coverage from keyless routed tables to indexed routed
tables. Renaming an indexed table must preserve table id, index metadata,
duplicate-key checks, indexed lookups, and close/reopen discovery under the new
table name.

## Non-Goals

- Do not implement SQL index renaming or `ALTER TABLE ... RENAME INDEX`.
- Do not change physical index-entry page format.
- Do not add transactional DDL rollback beyond the current statement
  checkpoint behavior.
- Do not add foreign-key, trigger, or partition rename semantics.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_table.cc:5544-5611` routes table renames through
  `handler::ha_rename_table()` when a handler rename is requested.
- `mariadb/sql/sql_table.cc:5611-5650` may rename high-level indexes after the
  base table rename. This slice does not need SQL-level index rename support;
  it verifies ordinary indexes remain attached to the renamed base table.
- `docs/specs/rename-table-catalog-lifecycle/specs.md` records the existing
  MyLite rename design: catalog identity changes while table id and definition
  roots stay stable.
- `docs/specs/primary-secondary-indexes/specs.md` records that MyLite index
  entries are stored by table id and index number, not by schema/table name.

## Compatibility Impact

This moves indexed table rename from an unproven planned gap to partial
coverage for supported primary, unique, and secondary index shapes:

- the old table name disappears;
- the new table name preserves indexed reads through `FORCE INDEX`;
- unique-key checks still reject duplicate key values after rename;
- reopened sessions discover the renamed table and its indexes.

`ALTER TABLE ... RENAME INDEX` and high-level index renames remain planned.

## Design

Extend the storage-engine smoke test with a separate indexed table:

1. Create an `ENGINE=InnoDB` table with primary, unique, and secondary keys.
2. Insert rows and verify the indexed path before rename through existing index
   coverage.
3. Rename the table through SQL.
4. Verify old-name rejection, new-name catalog metadata, indexed reads through
   the unique and secondary keys, and duplicate-key rejection.
5. Close and reopen the database, then repeat representative indexed reads
   through the renamed table.

No storage API change should be needed if the current table-id design is
correct. If the test reveals stale table-definition identity or index metadata,
fix the catalog/handler layer directly.

## File Lifecycle

The slice must keep durable state inside the primary `.mylite` file. Existing
sidecar gates should continue to reject `.frm`, `.ibd`, `.MYD`, `.MYI`, `.MAI`,
`.MAD`, Aria logs, binlogs, relay logs, and plugin-owned table files.

## Embedded Lifecycle And API

No public `libmylite` API change is required. The behavior is observed through
`mylite_exec()` and close/reopen discovery.

## Build, Size, And Dependencies

No new dependency, build-profile, or size-sensitive production code is expected.

## Test Plan

1. Extend `mylite_embedded_storage_engine_test` with indexed rename coverage.
2. Run the storage-engine compatibility group and full storage-smoke preset.
3. Run format, tidy, diff, dev, and embedded preset checks before commit.

## Acceptance Criteria

- Indexed routed table rename succeeds.
- Old name fails after rename.
- New name supports unique and secondary index reads before and after reopen.
- Duplicate unique-key checks still work after rename.
- Catalog metadata and compatibility docs no longer describe index rename paths
  as untested for ordinary table renames.

## Implementation Status

Implemented in the storage-engine smoke test:

- `mylite_embedded_storage_engine_test` now renames an indexed
  `ENGINE=InnoDB` table with primary, unique, and secondary keys.
- The test verifies old-name rejection, new-name catalog metadata, unique and
  secondary `FORCE INDEX` reads, duplicate unique-key rejection, close/reopen
  discovery, and sidecar cleanup.
- No production storage change was required because table id preservation
  already carries existing index-entry pages with the renamed table.

## Risks And Open Questions

- MariaDB's high-level index rename path is separate from preserving existing
  indexes across a table rename. This slice covers the latter only.
- If the restored table-definition blob embeds old table identity in a path that
  affects indexes, the implementation may need to rewrite more than catalog
  identity.

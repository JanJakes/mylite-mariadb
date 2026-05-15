# Temporary Table Shadowing

## Goal

Cover representative temporary `CREATE TABLE ... LIKE` and
`CREATE TABLE ... SELECT` statements that shadow durable MyLite-routed tables
with the same SQL name, then prove the durable tables become visible again
after `DROP TEMPORARY TABLE` and after close/reopen.

## Non-Goals

- Exhaustive temporary-table compatibility for all column types, locks,
  triggers, views, partitions, foreign keys, broader `OR REPLACE` variants, or
  duplicate modes.
- Dedicated non-durable temporary storage outside the current MyLite temporary
  handler lifecycle.
- SQL transaction, savepoint, or rollback semantics for temporary-table DDL.
- Public `libmylite` API or file-format changes.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/temporary_tables.cc:164-175` documents the temporary-table share
  lookup used to detect a temporary table that shadows a base table.
- `mariadb/sql/temporary_tables.cc:297-302` states that temporary tables are
  thread-local and shadow base tables with the same name.
- `mariadb/sql/temporary_tables.cc:322-440` resolves table references through
  the session's temporary-table list before falling back to base tables.
- `mariadb/sql/temporary_tables.cc:1198-1210` finds and marks a temporary table
  for use by its database and table name.
- `mariadb/sql/sql_table.cc:4700-4725` checks existing temporary tables by name
  during temporary create, without treating same-named base tables as create
  conflicts.
- `mariadb/sql/sql_table.cc:4947-4966` creates and opens temporary tables
  through `THD::create_and_open_tmp_table()` and stores the temporary TABLE in
  `create_info->table`.
- `mariadb/sql/sql_table.cc:5768-5777` applies the CREATE OR REPLACE
  self-source guard only to non-temporary `CREATE TABLE ... LIKE`.
- `mariadb/sql/sql_insert.cc:5161-5170` removes a temporary CTAS target from the
  THD temporary-table list while reading sources so statements like
  `CREATE TEMPORARY TABLE t1 AS SELECT * FROM t1` can read the original source.
- `mariadb/sql/sql_insert.cc:5417-5435` restores the temporary CTAS target after
  row insertion, unless an inner statement created the same temporary table.

## Compatibility Impact

Temporary `LIKE` and CTAS support remains partial, but MyLite no longer treats
same-name temporary/base-table shadowing as an uncovered temporary-table edge
case for representative supported table shapes. Broader temporary lock,
replacement, and unsupported-source cases remain planned; representative
temporary OR REPLACE behavior is covered by a dedicated follow-up slice.

## Design

Use MariaDB's normal temporary-table name resolution:

1. Create durable `ENGINE=InnoDB` tables routed to MyLite.
2. Create temporary `LIKE` and CTAS tables with the same SQL-visible names.
3. Verify ordinary SQL resolves to the temporary table while it exists.
4. Verify the durable MyLite catalog count and metadata do not change.
5. Drop the temporary tables and verify SQL resolves to the durable tables
   again.
6. Close and reopen the file, then verify only durable tables remain visible.

No MyLite handler changes should be necessary unless the existing temporary
storage identity collides with durable catalog records or fails cleanup.

## File Lifecycle

Temporary-table shadowing must not add durable catalog records for the
temporary SQL-visible names or create forbidden MariaDB sidecars. Any live
temporary storage identity must stay under the existing MyLite temporary
runtime lifecycle and be cleaned up by explicit drop or final close. Durable
base table rows and indexes must remain in the primary `.mylite` file.

## Embedded Lifecycle And API

No public API changes. The storage-smoke test covers same-session shadowing,
explicit temporary drop, close/reopen cleanup, and absence of runtime schema
directories after reopen.

## Build, Size, And Dependencies

No dependency or build-profile change. Binary-size impact is limited to test
and documentation unless a handler fix is required.

## Test Plan

- Extend storage-engine smoke coverage to:
  - create durable base tables with supported primary, unique, and secondary
    indexes;
  - create a temporary `LIKE` table with the same SQL name as one durable table;
  - create a temporary CTAS table with the same SQL name as another durable
    table;
  - verify temporary rows shadow durable rows while the temporary tables exist;
  - verify durable catalog metadata and table counts are unchanged;
  - drop the temporary tables and verify durable rows and indexes are visible
    again;
  - close/reopen and verify the durable rows remain visible and temporary names
    do not survive.
- Run focused storage-smoke coverage, compatibility reports for routed DDL/DML
  and sidecar groups, format, tidy, diff, shell checks, and full preset gates.

## Acceptance Criteria

- Temporary `LIKE` and CTAS tables can use the same SQL names as durable routed
  tables.
- SQL resolves to the temporary table while the temporary table exists and back
  to the durable table after `DROP TEMPORARY TABLE`.
- Durable catalog metadata and rows survive temporary shadowing and close/reopen.
- No forbidden durable sidecars or runtime schema directories are introduced.
- Compatibility, roadmap, storage architecture, and the temporary-table spec
  identify shadowing as covered representative behavior while keeping broader
  temporary-table variants planned.
- The dedicated temporary OR REPLACE slice verifies representative replacement
  behavior on top of this shadowing lifecycle.

## Risks And Open Questions

- The current temporary implementation may still append primary-file pages for
  temporary storage identities. This slice only proves SQL-visible shadowing and
  cleanup; physical reclamation remains a compaction concern.
- Temporary CTAS self-reference behavior is subtle because MariaDB temporarily
  hides the new target while reading sources. This slice covers shadowing a
  durable source name, not all self-reference variants.

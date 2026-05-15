# Temporary Table Catalog Isolation

## Problem

MyLite supports routed base-table `CREATE TABLE ... LIKE` and
`CREATE TABLE ... SELECT`, but their temporary-table variants are still marked
planned. Temporary tables matter for schema migrations, import tooling,
application setup flows, and direct SQL API behavior. MyLite needs explicit
coverage that user temporary tables remain session-local: they may use
MariaDB's temporary-table handler lifecycle while open, but they must not
become durable user catalog tables in the `.mylite` file.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.h:506` defines `HA_LEX_CREATE_TMP_TABLE`, and
  `mariadb/sql/handler.h:2316-2320` exposes `HA_CREATE_INFO::tmp_table()` and
  chooses `ha_default_tmp_handlerton(thd)` for temporary tables.
- `mariadb/sql/handler.cc:210-246` falls back from the temporary-table engine
  to the normal default engine when no explicit `default_tmp_storage_engine`
  exists.
- `mariadb/sql/sql_table.cc:13364-13413` applies
  `@@enforce_storage_engine` during create planning, then rejects temporary
  tables only if the chosen engine has `HTON_TEMPORARY_NOT_SUPPORTED`.
- `mariadb/sql/sql_table.cc:4947-5050` creates and opens temporary tables
  through `THD::create_and_open_tmp_table()` after the handler create call.
- `mariadb/sql/sql_table.cc:mysql_create_like_table()` keeps the caller's
  `HA_LEX_CREATE_TMP_TABLE` flag on the cloned create-info, resets cloned
  autoincrement state, and calls the normal no-lock create path.
- `mariadb/sql/sql_insert.cc:select_create::create_table_from_items()` stores
  the opened temporary target table in `create_info->table` for temporary
  CTAS, then `select_create::prepare()` removes it temporarily from the THD
  temporary-table list while reading source tables.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::create()` publishes any
  supported handler create path through `mylite_storage_store_table_definition`.
  For temporary-table paths this uses MariaDB's temporary storage identity,
  not the SQL-visible user table name.

## Scope

- `CREATE TEMPORARY TABLE temp LIKE source` over a supported MyLite-routed
  source table.
- `CREATE TEMPORARY TABLE temp AS SELECT ...` over a supported MyLite-routed
  source table.
- Same-session temporary table shadowing of a durable MyLite-routed table with
  the same SQL-visible name for representative LIKE and CTAS paths.
- Temporary row insert/read behavior sufficient to prove the temp table is
  usable during the session.
- MyLite catalog isolation for SQL-visible temporary table names in the user
  schema, plus cleanup after explicit `DROP TEMPORARY TABLE` and close/reopen.

## Non-Goals

- New storage file-format primitives for a dedicated in-memory temp-table
  store.
- Claiming full temp-table compatibility for all column types, indexes,
  `OR REPLACE`, `IGNORE` / `REPLACE`, locks, views, partitions, or foreign
  keys.
- Physical compaction of table-definition, row, or index pages orphaned by
  normal drop-style lifecycle cleanup.
- Changing public `libmylite` APIs.

## Design

Use MariaDB's existing temporary-table paths rather than adding a parallel
temporary-table executor:

1. Keep file-backed MyLite sessions enforcing the MyLite handler so routed temp
   table behavior exercises the same SQL semantics as base tables.
2. Let `CREATE TEMPORARY TABLE ... LIKE` and temporary CTAS reach
   `ha_mylite::create()` through the existing MariaDB create paths.
3. Treat the handler's temporary storage identity as session-local runtime
   state. The SQL-visible user schema must not gain durable catalog records for
   the temporary names.
4. Rely on explicit `DROP TEMPORARY TABLE` and MariaDB close cleanup to remove
   any live temporary storage-identity records through `ha_mylite::delete_table`.
5. Let MariaDB temporary-table name resolution shadow same-named durable base
   tables while the temporary table exists.
6. Keep close/reopen discovery limited to durable base tables.

This is intentionally a lifecycle coverage slice. A later temp-storage slice
can replace temporary handler records with a dedicated non-durable table store
if the current normal create/drop path proves too much file churn or too much
fork maintenance cost.

## Compatibility Impact

`CREATE TEMPORARY TABLE ... LIKE` and supported temporary CTAS move from
planned to partial. The support claim is limited to representative MyLite-routed
source tables, basic row visibility during the creating session, and
representative temporary-table shadowing.

## DDL Metadata Routing Impact

Temporary SQL-visible table names do not become MyLite catalog tables in the
user schema. The source base table remains the only durable table record after
explicit temporary-table drops and after close/reopen.

## Single-File And Embedded Lifecycle Impact

No persistent MariaDB sidecars are introduced. Temporary-table runtime state
stays under the normal MyLite primary-file and temporary-runtime lifecycle, with
logical catalog cleanup verified before close and after reopen. Physical page
reclamation remains a separate compaction concern already shared with ordinary
`DROP TABLE`.

## Public API And File-Format Impact

No public `libmylite` API change and no storage file-format change.

## Storage-Engine Routing Impact

File-backed sessions keep resolving omitted engines and compatible engine
requests to the MyLite handler. `ENGINE=InnoDB` source tables still execute
through MyLite while preserving requested-engine metadata for durable tables.

## Wire-Protocol And Integration-Package Impact

No wire-protocol package or external integration change.

## Binary-Size And Dependency Impact

No dependency is added. Binary-size impact should be limited to storage-smoke
test code unless handler fixes are needed.

## Test And Verification Plan

- Add storage-engine smoke coverage that:
  - creates a durable `ENGINE=InnoDB` source table;
  - creates `CREATE TEMPORARY TABLE temp_like LIKE source`;
  - inserts and reads rows from the temporary LIKE table;
  - creates `CREATE TEMPORARY TABLE temp_select AS SELECT ... FROM source`;
  - verifies CTAS rows are visible during the session;
  - creates same-name temporary LIKE and CTAS tables that shadow durable tables;
  - verifies SQL resolves to temporary rows while the temporary tables exist and
    durable rows after `DROP TEMPORARY TABLE`;
  - verifies neither temporary SQL-visible name exists in the durable user
    schema catalog;
  - drops both temporary tables and verifies no temporary storage-identity
    catalog records remain in the runtime temp namespace;
  - leaves one temporary table open across `mylite_close()` and verifies close
    cleanup removes its live temporary storage record;
  - closes and reopens, then verifies the source table remains durable and the
    temporary tables are gone.
- Run targeted storage-smoke tests, compatibility reports for routed DDL/DML
  and sidecar groups, format, tidy, diff, shell checks, and full preset gates.

## Acceptance Criteria

- Supported temporary `LIKE` and CTAS statements succeed in file-backed MyLite
  storage-smoke coverage.
- Temporary rows are readable during the creating session.
- The durable user schema catalog contains only durable base tables.
- Same-name temporary LIKE and CTAS tables shadow durable base tables only until
  `DROP TEMPORARY TABLE`.
- Temporary tables are not visible after close/reopen.
- Compatibility, roadmap, and related specs stop listing the covered temporary
  variants as completely planned.

## Implementation Status

Implemented in storage-engine smoke coverage:

- `CREATE TEMPORARY TABLE temp_like LIKE source` clones a routed
  `ENGINE=InnoDB` source shape and supports temporary insert/read operations.
- `CREATE TEMPORARY TABLE temp_select AS SELECT ...` copies representative
  source rows into a session-local temporary target.
- Same-name temporary LIKE and CTAS tables shadow durable tables during the
  session, then durable rows and indexes become visible again after
  `DROP TEMPORARY TABLE`.
- The SQL-visible temporary names never appear as durable user-schema catalog
  records, explicit `DROP TEMPORARY TABLE` and close-time cleanup remove live
  temporary storage records from the runtime temp namespace, and close/reopen
  keeps only the durable source table visible.

## Risks And Unresolved Questions

- The current handler may still use ordinary primary-file pages for temporary
  storage identities while the table is open. That is acceptable for this
  partial support slice only if live catalog cleanup is verified; a dedicated
  non-durable temp store remains a possible future improvement.
- MariaDB temporary table storage identities are implementation details. Tests
  should only depend on the current MyLite runtime temp namespace when checking
  cleanup, and broader platform path variants remain a future compatibility
  concern.

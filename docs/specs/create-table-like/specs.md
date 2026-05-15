# CREATE TABLE LIKE

## Problem

MyLite routes ordinary `CREATE TABLE`, copy `ALTER`, `RENAME`, `DROP`,
standalone index DDL, and `TRUNCATE`. `CREATE TABLE ... LIKE` is the next
metadata DDL gap. Real schemas use it to clone table shape without copying
rows. For MyLite, the cloned definition must stay catalog-backed, must not
create durable MariaDB sidecars, and must preserve requested-engine metadata
where the source table was routed to effective `MYLITE`.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_lex.h:4604-4621` distinguishes `CREATE TABLE ... LIKE`
  from `CREATE TABLE ... SELECT` and simple `CREATE TABLE` through
  `LEX::create_like()`, `LEX::create_select()`, and `LEX::create_simple()`.
- `mariadb/sql/structs.h:590-603` represents `CREATE TABLE LIKE` as
  `DDL_options_st::OPT_LIKE` and keeps only `IF NOT EXISTS` / `OR REPLACE`
  options when building the cloned table create-info.
- `mariadb/sql/sql_table.cc:mysql_create_like_table()` opens the source table,
  calls `mysql_prepare_alter_table()` to build a cloned `Alter_info`, resets
  the cloned autoincrement value, drops inherited data/index directory
  attributes, and calls `mysql_create_table_no_lock()` for the target table.
- `mariadb/sql/sql_table.cc:Sql_cmd_create_table_like::execute()` routes the
  no-select `CREATE TABLE ... LIKE` branch to `mysql_create_like_table()`;
  `CREATE TABLE ... SELECT` uses a separate `select_create` path.
- `mariadb/storage/mylite/ha_mylite.cc` currently preserves requested-engine
  metadata for copy rebuilds and standalone index DDL, but no-engine
  `CREATE TABLE ... LIKE` targets would otherwise be cataloged as requested
  `DEFAULT` even when the source table was requested as `InnoDB`, `MyISAM`, or
  `Aria` and routed to effective `MYLITE`.
- MariaDB's `CREATE TABLE` documentation describes `CREATE TABLE ... LIKE` as
  creating an empty table using the same table definition as another table:
  <https://mariadb.com/docs/server/server-usage/tables/create-table>.

## Scope

- Plain routed `CREATE TABLE target LIKE source` over supported MyLite base
  tables.
- Clone table definitions, supported primary/unique/secondary indexes,
  bounded BLOB/TEXT prefix indexes, nullable columns, BLOB/TEXT columns, and
  autoincrement declarations.
- Preserve source requested-engine metadata when the statement does not specify
  a new engine and the source is a MyLite-routed table.
- Keep target tables empty and reset target autoincrement state.
- Insert, duplicate-key checks, forced-index lookup, close/reopen metadata, and
  durable-sidecar gates for cloned tables.

## Non-Goals

- `CREATE TABLE ... SELECT`, which has separate row-copy and statement rollback
  behavior.
- Temporary `CREATE TEMPORARY TABLE ... LIKE` and `CREATE OR REPLACE TABLE ...
  LIKE` lock-table edge cases.
- Cloning views, information-schema tables, partitions, foreign keys, triggers,
  or unsupported index classes.
- SQL rollback, savepoints, or transaction-aware DDL rollback.

## Design

Use MariaDB's existing `mysql_create_like_table()` path. MyLite does not need a
separate clone executor:

1. MariaDB opens the source table and builds the cloned table definition.
2. MyLite receives the target definition through `ha_mylite::create()`.
3. If the target statement has no explicit engine and `LEX::create_like()` is
   true, read the requested-engine metadata from the source table catalog
   record instead of storing `DEFAULT`.
4. Keep explicit engine requests on the existing `HA_CREATE_USED_ENGINE` path.
5. Store the cloned binary table definition in the MyLite catalog and let
   normal row/index write paths populate the cloned table later.

This keeps MariaDB responsible for table-definition semantics while MyLite owns
only routing metadata and single-file publication.

## Compatibility Impact

`CREATE TABLE ... LIKE` moves from planned to partial for supported routed base
tables. It remains partial because unsupported source objects, temporary-table
variants, foreign keys, partitions, and SQL rollback need separate slices.

## DDL Metadata Routing Impact

The cloned target becomes a normal MyLite catalog record with source requested
engine metadata and effective `MYLITE`. Unsupported cloned key classes must
still fail before catalog publication.

## Single-File And Embedded Lifecycle Impact

Successful clones add only MyLite catalog definition pages inside the primary
`.mylite` file. No `.frm`, `.ibd`, `.MYD`, `.MYI`, `.MAI`, `.MAD`, Aria log,
binlog, relay log, or plugin-owned durable table file is introduced. Existing
rollback-journal and sidecar cleanup behavior remains in force.

## Public API And File Format Impact

No public `libmylite` API change and no storage file-format change.

## Storage-Engine Routing Impact

The support applies through the MyLite handler for source tables requested as
`MYLITE`, omitted/default, `InnoDB`, `MyISAM`, or `Aria` when their cloned
shape is otherwise supported. External durable engine files remain out of
scope.

## Binary-Size And Dependency Impact

No dependency is added. Binary-size impact should be limited to handler branch
logic and smoke-test code; update size measurements after verification.

## Test Plan

- Extend storage-engine smoke coverage for:
  - `CREATE TABLE cloned LIKE source` over a routed `ENGINE=InnoDB` source;
  - no source row copy and reset autoincrement state;
  - cloned unique and BLOB/TEXT prefix indexes;
  - duplicate-key checks and forced-index lookup on inserted target rows;
  - close/reopen source and clone metadata;
  - durable-sidecar gates.
- Run dev, embedded-dev, storage-smoke-dev, format, tidy, diff, shell,
  compatibility harness, and size checks.

## Acceptance Criteria

- Supported `CREATE TABLE ... LIKE` succeeds for routed MyLite source tables.
- The target table is empty immediately after creation.
- The target table's requested-engine metadata matches the source requested
  engine when no explicit target engine is specified.
- Inserted target rows use the cloned key definitions for duplicate checks and
  forced-index reads before and after close/reopen.
- Compatibility, roadmap, and storage architecture docs describe
  `CREATE TABLE ... LIKE` as partial support with explicit remaining limits.

## Implementation Status

Implemented in the MyLite handler and storage-engine smoke:

- `mylite_preserves_requested_engine_name()` includes no-engine
  `SQLCOM_CREATE_TABLE` statements where `LEX::create_like()` is true.
- `mylite_preserve_source_requested_engine_name()` selects the LIKE source
  table from `LEX::create_last_non_select_table->next_global` and reads its
  requested-engine metadata before publishing the target catalog record.
- Storage-engine smoke covers an `ENGINE=InnoDB` source table, empty cloned
  target, reset target autoincrement, cloned unique and BLOB/TEXT prefix
  indexes, duplicate-key checks, forced-index reads, close/reopen metadata, and
  durable-sidecar gates.

## Risks And Open Questions

- `CREATE TABLE ... LIKE` has separate temporary-table and `OR REPLACE`
  branches in MariaDB. This slice keeps those outside the support claim until
  lock-table and temporary lifecycle coverage exists.
- Source-table selection in the MyLite handler must match MariaDB's `LEX`
  layout for `CREATE TABLE ... LIKE`; using the target table would silently
  store the wrong requested-engine metadata.

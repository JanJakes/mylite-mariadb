# CREATE TABLE SELECT

## Problem

MyLite now supports routed table creation, table cloning, row writes, supported
indexes, BLOB/TEXT payloads, and close/reopen discovery, but
`CREATE TABLE ... SELECT` needs explicit coverage. CTAS is common in schema migration,
reporting, and application setup flows. The first MyLite support should prove
successful CTAS over supported routed table shapes without claiming full
transactional failure semantics.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_lex.h:4604-4621` distinguishes `CREATE TABLE ... SELECT`
  through `LEX::create_select()`, separate from `CREATE TABLE ... LIKE` and
  simple `CREATE TABLE`.
- `mariadb/sql/sql_table.cc:Sql_cmd_create_table_like::execute()` routes CTAS
  through the `select_create` result path: it opens source tables, unlinks the
  target from the SELECT namespace, creates a `select_create`, and calls
  `handle_select()`.
- `mariadb/sql/sql_insert.cc:select_create::create_table_from_items()` derives
  table fields from selected items, appends them to `Alter_info`, calls
  `fix_create_fields()`, and creates/opens the target table.
- `mariadb/sql/sql_insert.cc:select_create::prepare()` sets the target write
  set, prepares duplicate handling, starts bulk insert hints, and constructs
  `Write_record` for row writes into the target handler.
- `mariadb/sql/sql_insert.cc:select_create::store_values()` fills the target
  record from each selected row before the normal insert/write path.
- `mariadb/sql/sql_insert.cc:select_create::abort_result_set()` and
  `mariadb/sql/sql_base.cc:drop_open_table()` show that CTAS has explicit
  abort/drop handling in MariaDB; MyLite should not claim rollback behavior
  beyond the currently implemented non-transactional handler path.
- MariaDB's `CREATE TABLE` documentation describes CTAS forms where the table
  can be created from a `SELECT` result:
  <https://mariadb.com/docs/server/server-usage/tables/create-table>.

## Scope

- Successful non-temporary `CREATE TABLE target AS SELECT ...` and
  `CREATE TABLE target SELECT ...` over MyLite-routed sources and constant
  expressions.
- Explicit supported target engines such as `ENGINE=InnoDB`, plus no-explicit
  engine targets routed to effective `MYLITE`.
- CTAS with derived columns, BLOB/TEXT expressions, and existing source rows.
- CTAS projections that read virtual and stored generated columns from
  MyLite-routed source tables into ordinary target columns.
- CTAS with an explicit supported target key declaration before `SELECT`, so
  inserts exercise duplicate checks and index-entry publication.
- Close/reopen metadata, row visibility, forced-index reads, and durable-sidecar
  gates.

## Non-Goals

- Physical rollback of row, overflow, index-entry, or autoincrement pages
  written before failed CTAS abort.
- `IGNORE` / `REPLACE` CTAS and lock-table edge cases.
- Broader `CREATE OR REPLACE ... SELECT` edge cases beyond the representative
  successful replacement covered by `create-or-replace-table`.
- Broader temporary CTAS edge cases beyond the representative catalog
  isolation covered by `temporary-table-catalog-isolation`.
- Generated-column definitions on CTAS targets, CTAS from views, information
  schema, foreign keys, partitions, unsupported indexes, or server-only
  sources.
- SQL transaction commit/rollback, savepoints, or transaction-aware index
  maintenance.

## Design

Use MariaDB's existing `select_create` path. MyLite should not implement a
parallel CTAS executor:

1. Let MariaDB derive the target table definition from explicit create fields
   and selected items.
2. Let `ha_mylite::create()` publish the target catalog definition with the
   requested engine from the CTAS statement, or `DEFAULT` when no engine was
   specified.
3. Reuse existing `write_row()`, BLOB/TEXT row serialization, autoincrement,
   duplicate-key checks, and index-entry publication for the inserted SELECT
   rows.
4. Keep unsupported target shapes rejected before or during target publication
   through existing create/index support predicates.

The first support claim is successful-statement compatibility. Rollback on
mid-statement failure remains a separate transaction/DDL cleanup slice.

## Compatibility Impact

`CREATE TABLE ... SELECT` moves from planned to partial for supported routed
table shapes. Explicit CHECK-constrained targets, representative temporary CTAS
catalog isolation, and representative successful OR REPLACE CTAS are covered by
follow-up slices. It remains partial because physical rollback of pages written
before failed CTAS abort, failed replacement rollback, `IGNORE` / `REPLACE`,
broader temporary-table CTAS, foreign keys, partitions, unsupported source
objects, and SQL rollback remain planned.

## DDL Metadata Routing Impact

Successful CTAS creates a normal MyLite catalog record before rows are inserted.
The target requested engine is the explicit CTAS engine when present, otherwise
`DEFAULT`, with effective `MYLITE`.

## Single-File And Embedded Lifecycle Impact

Successful CTAS writes the target definition, rows, overflow payloads,
autoincrement state, and index-entry pages to the primary `.mylite` file. No
durable `.frm`, `.ibd`, `.MYD`, `.MYI`, `.MAI`, `.MAD`, Aria log, binlog,
relay log, or plugin-owned table file is introduced. Existing rollback-journal
and sidecar cleanup behavior remains in force for currently implemented
publication paths.

## Public API And File Format Impact

No public `libmylite` API change and no storage file-format change.

## Storage-Engine Routing Impact

The target table routes through the MyLite handler for omitted/default,
`MYLITE`, `InnoDB`, `MyISAM`, and `Aria` requests when the resulting table
shape is otherwise supported. Source tables are read through their existing
handlers; this slice only claims MyLite-routed sources and constants.

## Binary-Size And Dependency Impact

No dependency is added. Binary-size impact should be limited to smoke-test code
unless handler fixes are needed; update size measurements after verification.

## Test Plan

- Extend storage-engine smoke coverage for:
  - `CREATE TABLE ... AS SELECT` from constants and MyLite source rows;
  - explicit `ENGINE=InnoDB` CTAS requested/effective metadata;
  - no-engine CTAS requested `DEFAULT` metadata;
  - BLOB/TEXT payloads selected into the target;
  - virtual and stored generated source columns projected into ordinary target
    columns;
  - explicit generated target definitions;
  - explicit CHECK-constrained target definitions;
  - explicit target primary/unique/secondary indexes before `SELECT`;
  - duplicate-key checks and forced-index reads after CTAS;
  - close/reopen metadata and rows;
  - durable-sidecar gates.
- Run dev, embedded-dev, storage-smoke-dev, format, tidy, diff, shell,
  compatibility harness, and size checks.

## Acceptance Criteria

- Supported successful CTAS statements create MyLite catalog records and insert
  SELECT result rows.
- Target metadata records the requested engine correctly.
- CTAS target rows, BLOB/TEXT payloads, generated-source projections,
  generated target definitions, autoincrement values, and supported indexes
  survive close/reopen.
- Failed CTAS duplicate-key abort removes the target catalog metadata before
  the statement returns an error.
- Compatibility, roadmap, and storage architecture docs describe CTAS as
  partial support with explicit remaining limits.

## Implementation Status

Implemented through MariaDB's existing `select_create` path without new handler
entry points:

- Simple no-engine `CREATE TABLE ... AS SELECT` routes to MyLite and records
  requested `DEFAULT` with effective `MYLITE`.
- Explicit `ENGINE=InnoDB` CTAS with target primary, unique, and secondary
  indexes writes result rows through `ha_mylite::write_row()`.
- Storage-engine smoke covers selected BLOB/TEXT payloads, duplicate-key checks
  after CTAS, duplicate-key CTAS abort target cleanup, autoincrement
  advancement from copied rows, generated-source projections, explicit
  generated and CHECK-constrained target definitions, forced-index reads,
  close/reopen metadata and rows, and durable-sidecar gates.
- The `temporary-table-catalog-isolation` slice covers representative
  `CREATE TEMPORARY TABLE ... AS SELECT` behavior and verifies the SQL-visible
  temporary name does not become a durable user-schema catalog table.
- The `create-or-replace-table` slice covers representative successful
  `CREATE OR REPLACE TABLE ... SELECT` replacement over routed MyLite tables.

## Risks And Open Questions

- MariaDB has explicit CTAS abort/drop handling. MyLite follows that path for
  target catalog cleanup, but it still leaves any pages written before abort
  orphaned until SQL rollback, DDL undo, and compaction are implemented.
- CTAS can derive many SQL types from expressions. This slice should cover
  representative supported types and leave broad type-matrix comparison to a
  later MariaDB/MTR-scale compatibility effort.
- Generated-column definitions on CTAS targets are covered for the explicit
  base-column projection shape; broader target-expression matrices remain
  planned.

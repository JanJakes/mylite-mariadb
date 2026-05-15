# Generated Column CTAS Projection

## Problem

MyLite supports generated columns on routed tables and supports successful
`CREATE TABLE ... SELECT` for ordinary source columns. The remaining practical
gap is CTAS where the source projection reads generated columns. Schema
migration and reporting flows can materialize generated values into ordinary
target columns, so MyLite should prove that MariaDB computes those values before
the target rows are written through the MyLite handler.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_table.cc`: `CREATE TABLE ... SELECT` constructs a
  `select_create` result and runs the SELECT through `handle_select()`.
- `mariadb/sql/sql_insert.cc`: `select_create::create_table_from_items()`
  derives target fields from SELECT items, then `select_create::store_values()`
  fills the target record before normal handler writes.
- `mariadb/sql/table.cc`: `TABLE::update_virtual_fields()` computes virtual
  generated columns when they are read, and stored generated columns are stored
  in the source row payload.
- `mariadb/storage/mylite/ha_mylite.cc`: MyLite CTAS target rows are inserted
  through the same `write_row()` path as ordinary inserts.

## Scope

- CTAS from MyLite-routed source tables that include virtual and stored
  generated columns.
- Projection of generated values into ordinary columns on an `ENGINE=InnoDB`
  MyLite-routed CTAS target.
- Close/reopen visibility, catalog metadata, and sidecar gates for the source
  and target tables.

## Non-Goals

- Declaring generated columns on the CTAS target table.
- CTAS from views, information schema, foreign keys, partitions, unsupported
  index classes, or server-only sources.
- `CREATE OR REPLACE`, temporary CTAS, `IGNORE` / `REPLACE` CTAS, or lock-table
  variants.
- Dump/import fixtures, SQL rollback, savepoints, or broad expression-matrix
  comparison.

## Design

No MyLite CTAS executor or expression evaluator is introduced. The supported
path is:

1. MariaDB reads the generated source columns through its normal generated
   column machinery.
2. `select_create` derives ordinary target fields from the SELECT list.
3. MariaDB stores the computed generated values into the target record.
4. MyLite persists the CTAS target definition and rows through existing catalog
   and row append paths.

## Compatibility Impact

Generated-column support expands from direct DML, close/reopen, generated-index
paths, and copy ALTER to include CTAS projections from generated source
columns. Generated-column definitions on CTAS targets, dump/import,
transactional rollback, and broader expression matrices remain planned.

## DDL Metadata Routing Impact

The CTAS target is a normal MyLite table whose columns are ordinary fields
derived from the SELECT list. The generated metadata remains only on the source
table definition.

## Single-File And Embedded-Lifecycle Impact

Successful CTAS writes the generated-source table, generated-projection target,
and target rows to the primary `.mylite` file. It must not require persistent
MariaDB schema directories or durable engine sidecars after close/reopen.

## Public API, File-Format, Size, And Dependency Impact

No public `libmylite` API, file-format, binary-size-sensitive runtime, or
dependency change is expected. The slice adds compatibility smoke coverage and
documentation only unless the existing handler path needs a fix.

## Test And Verification Plan

- Extend storage-engine smoke CTAS coverage with a source table containing one
  virtual and one stored generated column.
- Insert source rows, run an `ENGINE=InnoDB` CTAS that selects generated
  columns, and verify materialized target values.
- Verify source and target catalog metadata before and after close/reopen.
- Verify generated projection rows after close/reopen.
- Run generated-column, routed DDL/DML, sidecar, format, tidy, preset, and diff
  checks.

## Acceptance Criteria

- CTAS can project virtual and stored generated source columns into ordinary
  target columns.
- The projected generated values are visible before and after close/reopen.
- No durable MariaDB sidecars or runtime schema directories are required after
  reopen.
- Compatibility docs distinguish generated-source CTAS projection from
  generated CTAS target definitions and dump/import work.

## Risks And Open Questions

- This slice proves the source-projection path, not generated metadata on the
  CTAS target.
- Broader generated expression matrices may reveal source read behavior that
  requires more MariaDB comparison coverage.

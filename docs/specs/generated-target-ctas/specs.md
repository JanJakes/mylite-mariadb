# Generated Target CTAS

## Problem

MyLite covers generated-source CTAS projections, where virtual and stored
generated values are selected into ordinary target columns. The remaining
generated CTAS gap is explicit generated columns declared on the target table
itself. That shape matters for migrations that create and seed a table in one
statement while preserving generated-column metadata.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_insert.cc`: `select_create::create_table_from_items()`
  appends SELECT-derived fields to `alter_info->create_list` before the target
  table is created.
- `mariadb/sql/sql_table.cc`: `mysql_prepare_create_table_finalize()` handles
  CTAS duplicate field names by redefining SELECT fields from explicit
  `CREATE TABLE (...)` field definitions, while leaving explicit target-only
  fields in the target definition.
- `mariadb/sql/field.cc`: `Column_definition::redefine_stage1_common()` copies
  `vcol_info` from explicit target field definitions when SELECT aliases match
  explicit base columns.
- `mariadb/sql/sql_insert.cc`: `select_create::store_values()` delegates to
  `fill_record_n_invoke_before_triggers()`, so generated values are evaluated
  through MariaDB's normal insert path before handler writes.
- `mariadb/storage/mylite/ha_mylite.cc`: MyLite already advertises virtual
  column support and persists generated-column metadata in the catalog-backed
  table-definition image.

## Scope

- Successful `CREATE TABLE ... SELECT` with explicit virtual and stored
  generated columns on the target table.
- SELECT supplies only base target columns; generated target columns are
  computed by MariaDB.
- Generated target values are readable before and after close/reopen.
- Existing sidecar and catalog metadata gates continue to pass.

## Non-Goals

- Generated primary keys, which MariaDB rejects before handler publication.
- Generated target indexes, generated BLOB/TEXT target payloads, or broad
  generated expression matrices.
- CTAS with `IGNORE`, `REPLACE`, temporary tables, views, foreign keys,
  partitions, unsupported index classes, or trigger/routine interactions.
- SQL transaction rollback beyond existing failed-statement checkpoints.

## Design

No MyLite handler change is required. The supported path is:

1. MariaDB merges SELECT aliases for base columns with explicit target field
   definitions.
2. Explicit generated target columns remain in the table definition.
3. MariaDB computes generated values during CTAS row insertion.
4. MyLite stores the resulting table-definition metadata and row data in the
   primary `.mylite` file.
5. Reopen rediscovers the generated target definition from the MyLite catalog.

## Compatibility Impact

Generated columns remain partial support, but generated target CTAS definitions
move from planned to covered for the explicit base-column projection shape.
Generated-source CTAS projection remains covered separately. Broader generated
expression matrices and broader dump/export fixtures remain planned.

## DDL Metadata Routing Impact

Successful generated target CTAS must publish a normal MyLite catalog table
whose table-definition image includes virtual and stored generated columns.

## Single-File And Embedded-Lifecycle Impact

No new page type or sidecar is introduced. Generated target metadata stays in
the catalog-backed table-definition image, and generated row values use the
existing row payload path.

## Public API, Size, And Dependency Impact

No public API, dependency, license, or binary-size impact is expected.

## Test And Verification Plan

- Extend storage-engine smoke CTAS coverage with explicit virtual and stored
  generated target columns.
- Verify generated values before close/reopen.
- Verify generated target metadata and values after close/reopen.
- Run generated-column, routed DDL/DML, sidecar, format, tidy, preset, shell
  syntax, and diff checks.

## Acceptance Criteria

- Generated target CTAS succeeds for supported base-column projections.
- Virtual and stored generated target values are readable before and after
  close/reopen.
- Compatibility docs and roadmap no longer list generated target CTAS
  definitions as an uncovered gap.

## Risks And Open Questions

- The covered shape depends on SELECT aliases matching explicit base target
  columns. Reordering, omitted required base columns, generated target indexes,
  and generated BLOB/TEXT target columns need separate coverage.
- MariaDB skips some expression validation paths when `create_simple()` is
  false; broader generated-expression matrices need explicit source and MTR
  evidence before support is expanded.

# CHECK Constraint CTAS

## Problem

CHECK constraints are covered for ordinary routed table DDL and row writes, but
the compatibility matrix still lists `CREATE TABLE ... SELECT` as a planned
CHECK surface. MyLite already routes CTAS rows through MariaDB's insert path, so
the next bounded step is to verify that explicit CHECK-constrained target
definitions are preserved and enforced during supported CTAS.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_insert.cc`: `select_create::create_table_from_items()`
  appends SELECT-derived fields to `alter_info->create_list` before table
  creation.
- `mariadb/sql/sql_table.cc`: `mysql_prepare_create_table_finalize()` handles
  CTAS duplicate field names by redefining SELECT fields with the explicit
  `CREATE TABLE (...)` field definitions.
- `mariadb/sql/field.cc`: `Column_definition::redefine_stage1_common()` copies
  `check_constraint` from the explicit target field definition to the
  SELECT-derived field.
- `mariadb/sql/sql_insert.cc`: `select_insert::send_data()` calls
  `TABLE_LIST::view_check_option()` before `write_record()`, and the base-table
  path delegates to CHECK verification.
- `mariadb/sql/table.cc`: `TABLE_LIST::view_check_option()` delegates to
  `TABLE::verify_constraints()` for base tables.
- `mariadb/sql/sql_insert.cc`: `select_create::abort_result_set()` drops the
  target after failed CTAS execution.

## Scope

- Successful `CREATE TABLE ... SELECT` with explicit column-level and
  table-level CHECK constraints on the target definition.
- Failed CTAS when SELECT-produced rows violate the target CHECK constraints.
- Catalog count, target absence after failed CTAS, row visibility, close/reopen,
  and sidecar checks.

## Non-Goals

- Generated target CTAS definitions.
- `CREATE TABLE ... SELECT` with `IGNORE`, `REPLACE`, temporary tables, views,
  triggers, foreign keys, partitions, or unsupported index classes.
- Broad CHECK expression matrices, dump/import fixtures, or prepared-statement
  diagnostics.
- Multi-statement transaction rollback beyond existing failed-statement
  checkpoints and MariaDB's CTAS target cleanup path.

## Design

No MyLite handler change is required. Keep using MariaDB's CTAS machinery:

1. explicit target field definitions carry CHECK metadata;
2. SELECT-derived fields with matching names are redefined from the explicit
   target definitions;
3. `select_insert::send_data()` verifies CHECK constraints before handler
   writes;
4. failed CTAS target cleanup removes the MyLite catalog table.

## Compatibility Impact

CHECK constraints remain partial support, but CTAS moves from planned to covered
for explicit target definitions whose SELECT output satisfies the constraints.
Generated target CTAS definitions and broader CHECK expression surfaces remain
planned.

## DDL Metadata Routing Impact

Successful CHECK CTAS must publish a normal MyLite catalog table with the target
definition metadata. Failed CHECK CTAS must not leave a target table visible in
the catalog or in `SHOW TABLES`.

## Single-File And Embedded-Lifecycle Impact

No file-format or companion-file change is introduced. CHECK metadata stays in
the catalog-backed MariaDB table-definition image. Failed CTAS and close/reopen
paths must not create durable MariaDB sidecars.

## Public API, Size, And Dependency Impact

No public API, dependency, license, or binary-size impact is expected.

## Test And Verification Plan

- Extend storage-engine smoke CTAS coverage with a successful explicit
  CHECK-constrained target.
- Add failed CTAS coverage where SELECT output violates a target CHECK and
  verify the target table is absent.
- Verify CHECK enforcement on later inserts before and after close/reopen.
- Run CHECK, routed DDL/DML, sidecar, format, tidy, preset, shell syntax, and
  diff checks.

## Acceptance Criteria

- Supported CHECK CTAS creates a MyLite-routed table and rows are visible.
- Violating CHECK CTAS fails without publishing target catalog metadata.
- Reopened CHECK CTAS metadata still rejects violating writes.
- Compatibility docs and roadmap mark CHECK CTAS as covered.

## Risks And Open Questions

- CHECK CTAS relies on MariaDB's field-redefinition path matching SELECT aliases
  to explicit target column names. Generated target definitions are deliberately
  left for a separate CTAS slice.
- Broader expression functions, disabled checks during CTAS, and dump/import
  remain to be covered separately.

# Constraint And Generated Expression Matrix

## Problem

MyLite covers basic CHECK constraints and generated columns, including ALTER,
CTAS, prepared diagnostics, generated indexes, and representative dump-style
import. The remaining expression gap is that current smoke coverage mostly
uses narrow arithmetic or length expressions. Applications often use simple
deterministic string, NULL-handling, conditional, and multi-column expressions
in constraints and generated columns.

This slice adds a representative expression matrix without claiming exhaustive
MariaDB expression compatibility.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc:mysql_prepare_create_table_finalize()` calls
  `check_expression()` for generated-column expressions during simple
  `CREATE TABLE` validation.
- `mariadb/sql/unireg.cc:pack_vcols()` packs generated-column and CHECK
  expressions into the table-definition image.
- `mariadb/sql/table.cc:parse_vcol_defs()` restores packed expression metadata
  when a table definition is reopened.
- `mariadb/sql/table.cc:TABLE::verify_constraints()` evaluates CHECK
  expressions unless `check_constraint_checks=OFF` is set.
- `mariadb/sql/table.cc:TABLE::update_virtual_fields()` computes virtual and
  stored generated-column values for read, write, update, and indexed paths.
- `mariadb/sql/sql_insert.cc` and `mariadb/sql/sql_update.cc` call
  `TABLE::update_virtual_fields()` before handler writes when generated fields
  must be materialized.
- MariaDB documentation describes CHECK constraints and generated-column
  expressions:
  <https://mariadb.com/docs/server/reference/sql-statements/data-definition/constraint>
  and
  <https://mariadb.com/docs/server/reference/sql-statements/data-definition/create/generated-columns>.

## Design

Add one storage-engine smoke table with routed `ENGINE=InnoDB` metadata and a
bounded expression set:

- CHECK constraints using `BETWEEN`, arithmetic across two columns, `IN`,
  `TRIM()`, `CHAR_LENGTH()`, `IS NULL`, and `OR`.
- Generated columns using `LOWER()`, `REPLACE()`, `CONCAT()`, `COALESCE()`,
  arithmetic, and `CASE`.
- A unique index on a virtual generated slug and a secondary index on a stored
  generated score.

The test inserts valid rows, verifies generated values, forces generated-index
reads, rejects representative CHECK failures, rejects a generated unique-key
duplicate, updates a base row to prove generated values and index entries move,
then closes and reopens the database and repeats key value, index, and CHECK
assertions.

## Supported Scope

- Representative deterministic CHECK expressions on routed base tables.
- Representative deterministic virtual and stored generated-column
  expressions.
- Generated-index reads and duplicate checks for the selected expressions.
- Update maintenance and close/reopen discovery for the selected expressions.

## Non-Goals

- Exhaustive MariaDB expression-function coverage.
- SQL-mode-sensitive expression matrices.
- JSON, spatial, full-text, stored-function, subquery, nondeterministic, or
  timezone-sensitive generated expressions.
- MySQL expression indexes.
- Full transaction or savepoint rollback for expression failures.

## Compatibility Impact

CHECK constraints and generated columns remain partial. The compatibility
matrix can claim a representative expression matrix for deterministic
CHECK/generated expressions while keeping exhaustive expression compatibility
planned.

## DDL Metadata Routing Impact

No MyLite-specific expression catalog is introduced. Expressions remain in the
MariaDB table-definition image stored by the MyLite catalog.

## Single-File And Embedded-Lifecycle Impact

The expression metadata, rows, generated stored values, and index entries stay
inside the primary `.mylite` file. Existing sidecar gates continue to prove
that no durable MariaDB metadata or engine files are required.

## Public API And File-Format Impact

No public `libmylite` API or file-format change is required.

## Storage-Engine Routing Impact

The test uses `ENGINE=InnoDB`, which resolves to effective `MYLITE` through the
existing routing policy.

## Binary-Size And Dependency Impact

No dependency or binary-size-sensitive runtime code is added. The slice adds
test and documentation coverage over existing MariaDB expression machinery.

## Test And Verification Plan

- Add storage-engine smoke coverage for the expression matrix.
- Update compatibility, storage architecture, roadmap, and related slice docs.
- Run format, targeted storage-smoke tests, CHECK/generated harness reports,
  tidy, full preset tests, shell checks, and `git diff --check`.

## Acceptance Criteria

- The routed expression matrix table publishes MyLite catalog metadata.
- Representative CHECK expression failures reject invalid inserts and updates.
- Representative generated values are readable before and after close/reopen.
- Generated-index reads and duplicate checks work before and after
  close/reopen.
- Docs distinguish representative expression-matrix coverage from exhaustive
  expression compatibility.

## Risks And Unresolved Questions

- Some MariaDB expression classes have SQL-mode or environment dependencies.
  This slice deliberately avoids those until a comparison matrix can define the
  expected compatibility target.
- Full expression coverage belongs in broader MTR-scale comparison, not in a
  single storage-smoke fixture.

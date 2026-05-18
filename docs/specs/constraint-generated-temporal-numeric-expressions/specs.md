# Constraint And Generated Temporal Numeric Expressions

## Problem

MyLite already has representative CHECK and generated-column expression
coverage for string, NULL-handling, arithmetic, and conditional expressions.
The remaining expression roadmap item is deliberately broader than a single
smoke fixture, but applications also commonly use deterministic numeric and
temporal expressions in generated columns and CHECK constraints. This slice
adds a bounded temporal/numeric matrix without claiming exhaustive expression
compatibility.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc:mysql_prepare_create_table_finalize()` validates
  generated-column and CHECK expressions with `check_expression()` during
  `CREATE TABLE`.
- `mariadb/sql/unireg.cc:pack_vcols()` stores generated-column and CHECK
  expression metadata in the table-definition image.
- `mariadb/sql/table.cc:parse_vcol_defs()` restores packed expression metadata
  when catalog-backed table definitions are reopened.
- `mariadb/sql/table.cc:TABLE::verify_constraints()` evaluates CHECK
  expressions before handler writes unless checks are disabled.
- `mariadb/sql/table.cc:TABLE::update_virtual_fields()` computes virtual and
  stored generated-column values for write and read paths.
- `mariadb/sql/sql_insert.cc` and `mariadb/sql/sql_update.cc` invoke virtual
  column maintenance before routed handler writes.
- MariaDB documentation describes generated columns and CHECK constraints at
  <https://mariadb.com/docs/server/reference/sql-statements/data-definition/create/generated-columns>
  and
  <https://mariadb.com/docs/server/reference/sql-statements/data-definition/constraint>.

## Design

Add a second storage-engine smoke expression table with routed `ENGINE=InnoDB`
metadata and deterministic temporal/numeric expressions:

- generated customer keys using `LOWER(TRIM(...))`,
- generated integer totals using arithmetic and `FLOOR()`,
- generated shipment intervals using `CASE` plus `TIMESTAMPDIFF()`,
- generated order months using `YEAR()` and `MONTH()`,
- CHECK constraints over non-negative amounts, bounded tax rates, shipment
  ordering, non-blank customers, and derived total caps,
- a unique generated-column index and secondary generated-column indexes.

The test inserts valid rows, verifies generated values and forced generated
index reads, rejects representative CHECK and duplicate generated-key failures,
updates a base row to prove generated values and generated indexes move, then
closes and reopens the file and repeats the key assertions.

## Supported Scope

- Representative deterministic numeric generated expressions.
- Representative deterministic temporal generated expressions.
- CHECK expressions that combine base numeric and temporal columns.
- Generated unique and secondary index maintenance across insert, update, and
  close/reopen.

## Non-Goals

- Exhaustive MariaDB expression-function coverage.
- SQL-mode-sensitive, locale-sensitive, timezone-sensitive, nondeterministic,
  stored-function, subquery, JSON, spatial, or full-text expression coverage.
- MySQL-style base-table expression indexes.
- Transaction or savepoint rollback beyond the already covered statement
  checkpoint behavior.
- Any MyLite-native expression evaluator.

## Compatibility Impact

CHECK constraints and generated columns remain partial. The compatibility
matrix can say representative deterministic expression matrices now include
string/NULL/conditional plus temporal/numeric expressions, while exhaustive
expression coverage remains planned.

## DDL Metadata Routing Impact

No new MyLite expression catalog is introduced. MariaDB-owned expression
metadata stays in the catalog-backed table-definition image.

## Single-File And Embedded-Lifecycle Impact

Definitions, rows, generated stored values, and generated index entries remain
inside the primary `.mylite` file. Existing sidecar gates continue to verify no
durable MariaDB metadata or engine sidecars are created.

## Public API And File-Format Impact

No public `libmylite` API or file-format change is required.

## Storage-Engine Routing Impact

The test uses explicit `ENGINE=InnoDB`, which resolves to effective `MYLITE`
through the existing routing policy.

## Binary-Size And Dependency Impact

No dependency or binary-size-sensitive runtime code is added.

## Test And Verification Plan

- Add storage-engine smoke coverage for the temporal/numeric expression table.
- Update compatibility, storage architecture, roadmap, and expression-related
  specs.
- Build `mylite_embedded_storage_engine_test`.
- Run the focused storage-engine smoke binary.
- Run `ctest --preset storage-smoke-dev`, `ctest --preset dev`, and
  `git diff --check`.

## Acceptance Criteria

- Routed table metadata publishes to the MyLite catalog.
- Generated numeric and temporal values are readable before and after reopen.
- Generated unique and secondary index reads work before and after reopen.
- CHECK failures reject invalid inserts and updates without publishing rows.
- Updating base columns updates generated values and generated index entries.
- Docs keep exhaustive expression compatibility marked as planned.

## Risks And Unresolved Questions

- MariaDB owns expression determinism and validation. This slice intentionally
  avoids functions with environment-sensitive semantics.
- Broader expression compatibility should be driven by MTR-scale comparison and
  targeted application evidence, not one storage-smoke fixture.

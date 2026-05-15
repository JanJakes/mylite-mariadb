# CHECK Constraint Coverage

## Goal

Cover MariaDB CHECK constraints for MyLite-routed tables where MariaDB can
evaluate the constraint expression before handler writes and MyLite can persist
the MariaDB table-definition image in the `.mylite` catalog.

This moves CHECK constraints from planned to partial support for basic
`CREATE TABLE`, insert, update, and reopen behavior.

## Non-Goals

- Implement a MyLite-native expression evaluator.
- Support foreign keys, generated columns, FULLTEXT, SPATIAL, or expression
  indexes.
- Implement transaction rollback for failed or disabled-check writes.
- Add broad MTR coverage for every MariaDB CHECK expression edge case.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_yacc.yy`: `check_constraint` parses `CHECK (...)`;
  `field_spec` accepts column-level checks and `constraint_def` accepts
  table-level named checks.
- `mariadb/sql/unireg.cc`: table-definition packing stores field and table
  check constraints in the `.frm` image.
- `mariadb/sql/table.cc`: table-open code reads check constraints from the
  table-definition image, and `TABLE::verify_constraints()` evaluates each
  expression unless `OPTION_NO_CHECK_CONSTRAINT_CHECKS` is set.
- `mariadb/sql/sql_insert.cc` and `mariadb/sql/sql_update.cc`: write paths call
  `TABLE_LIST::view_check_option()`, which delegates to
  `TABLE::verify_constraints()` for base tables.
- `mariadb/sql/sql_table.cc`: copy `ALTER` verifies constraints on rebuilt
  rows before writing them to the target table.
- `mariadb/sql/sys_vars.cc`: `check_constraint_checks` toggles
  `OPTION_NO_CHECK_CONSTRAINT_CHECKS`.
- Official MariaDB docs describe column-level and table-level CHECK
  constraints, evaluation before insert/update, and the
  `check_constraint_checks` variable:
  <https://mariadb.com/docs/server/reference/sql-statements/data-definition/constraint>
  and
  <https://mariadb.com/docs/server/reference/sql-statements/data-definition/create/create-table>.

## Compatibility Impact

CHECK constraints are SQL-layer semantics, not storage-engine-specific
referential metadata. Because MyLite stores the MariaDB table-definition image,
basic CHECK expressions can survive close/reopen and continue to be enforced by
MariaDB before the MyLite handler writes rows.

This slice marks CHECK constraints as partial support: covered for basic
column-level and table-level constraints on routed base tables, with broader
expression, ALTER, CTAS, dump-import, and rollback cases still planned.

## Design

Do not add new MyLite parser policy. Let MariaDB parse, store, reopen, and
evaluate CHECK constraints through the existing table-definition bridge.

Add storage-engine smoke coverage:

- create a routed `ENGINE=InnoDB` table with a column CHECK and a named table
  CHECK,
- reject inserts and updates that violate either constraint,
- allow a violating row while `check_constraint_checks=OFF` and verify the
  variable does not retroactively repair data when re-enabled,
- close/reopen and verify supported CHECK metadata still rejects new violating
  writes.

## File Lifecycle

CHECK metadata is part of the MariaDB table-definition image already stored in
the `.mylite` catalog. No new files, companions, or sidecars are introduced.
Existing sidecar gates remain the lifecycle proof.

## Embedded Lifecycle And API

`mylite_exec()` reports MariaDB constraint diagnostics for failed CHECK writes.
The stable MyLite result remains `MYLITE_ERROR` for now because constraint
error-code classification is broader than this slice.

Prepared statements inherit the same behavior through MariaDB execution; this
slice focuses on storage-smoke direct SQL coverage.

## Build, Size, And Dependencies

No new dependencies, no format change, and no meaningful binary-size impact.
The implementation is test and documentation coverage over existing MariaDB SQL
behavior plus MyLite catalog persistence.

## Test Plan

- Storage-engine smoke covers valid and invalid inserts on a checked
  `ENGINE=InnoDB` table.
- Storage-engine smoke covers invalid update rejection.
- Storage-engine smoke covers `SET check_constraint_checks=OFF/ON`.
- Storage-engine smoke covers close/reopen enforcement from catalog-backed
  table-definition metadata.
- Add a compatibility harness group for CHECK constraints.
- Run formatting, tidy, configured CTest presets, the named harness report, and
  `git diff --check`.

## Acceptance Criteria

- Basic CHECK constraints on routed tables reject invalid insert/update writes.
- CHECK metadata survives close/reopen through the `.mylite` catalog.
- `check_constraint_checks=OFF` behavior is documented and tested for this
  basic surface.
- Compatibility docs and roadmap mark CHECK constraints as partial rather than
  planned.
- The compatibility harness can run the CHECK evidence by name.

## Risks And Open Questions

- Complex deterministic functions, JSON-specific checks, strict-mode warning
  interactions, `IGNORE`, `LOAD DATA`, `ALTER TABLE ADD/DROP CONSTRAINT`,
  `CREATE TABLE ... SELECT`, and prepared-statement-specific diagnostics need
  later coverage.
- Failed statements still depend on current non-transactional rollback limits
  outside this slice.

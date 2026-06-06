# Ownerless View Check-Option DDL Crash

## Problem Statement

Ownerless view check-option refresh coverage proves already-open peers observe
`WITH CASCADED CHECK OPTION` and `WITH LOCAL CHECK OPTION` metadata and enforce
MariaDB errno 1369 for invalid DML through an updatable view. Hook-build crash
coverage covers simple view create/drop, replacement/altered definitions,
explicit column lists, idempotent no-ops, and bounded security metadata, but it
does not yet prove check-option metadata and enforcement survive a writer death
after MariaDB writes the native view definition file and before MyLite publishes
ownerless dictionary finish.

This slice adds deterministic crash-boundary evidence for check-option view
creation and replacement.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_yacc.yy`
  - `view_check_option` parses no option, `WITH CHECK OPTION`,
    `WITH CASCADED CHECK OPTION`, and `WITH LOCAL CHECK OPTION`.
- `mariadb/sql/sql_view.cc`
  - `mysql_create_view()` records whether a view is updatable.
  - `mysql_create_view()` rejects `WITH CHECK OPTION` on a non-updatable view.
  - `mysql_register_view()` stores `view->with_check` in the native view
    definition written by `sql_create_definition_file()`.
- `mariadb/sql/table.cc`
  - `TABLE_LIST::view_check_option()` raises `ER_VIEW_CHECK_FAILED` when DML
    through a view violates the active predicate.
- `packages/libmylite/src/database.cc`
  - Ownerless dictionary DDL classification treats `CREATE`, `ALTER`, and
    `DROP` statements as dictionary-generation boundaries, so check-option
    view metadata uses the same `dictionary-before-finish` unsafe hook as other
    view crash tests.

## Design

Add two unsafe-hook selectors to `mylite_ownerless_cross_process_sql_test`:

- `dictionary-view-check-option-create-crash` creates an InnoDB base table,
  kills a writer after `CREATE VIEW ... WITH CASCADED CHECK OPTION` writes the
  native view definition but before ownerless dictionary finish, then verifies
  recovered `CHECK_OPTION='CASCADED'`, `IS_UPDATABLE='YES'`, valid DML, and
  MariaDB errno 1369 for invalid DML.
- `dictionary-view-check-option-replace-crash` creates an initial cascaded
  check-option view, kills a writer after `CREATE OR REPLACE VIEW ... WITH
  LOCAL CHECK OPTION` rewrites the native view definition but before ownerless
  dictionary finish, then verifies recovered `CHECK_OPTION='LOCAL'`, the
  replacement predicate, valid DML, and MariaDB errno 1369 for invalid DML.

Both selectors verify ownerless and ordinary native reopen before and after a
forced `.shm` rebuild.

## Scope

In scope:

- Crash-at-`dictionary-before-finish` coverage for check-option view creation.
- Crash-at-`dictionary-before-finish` coverage for check-option view
  replacement.
- Recovered `.frm` presence under `datadir/app/`.
- Recovered `INFORMATION_SCHEMA.VIEWS.CHECK_OPTION` and `IS_UPDATABLE`
  metadata.
- Query behavior and valid DML through the recovered updatable view.
- Invalid insert/update failure with MariaDB errno 1369.
- Base-table writes after recovery.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- `ALTER VIEW ... WITH CHECK OPTION` crash coverage.
- Nested view local-versus-cascaded propagation crash coverage.
- Invalid dependencies, non-updatable view diagnostics, prepared statements,
  privilege/security behavior, or invalid definers.
- External randomized DDL/RQG stress.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens ownerless compatibility
evidence by proving completed MariaDB check-option view metadata changes
survive a writer death at MyLite's dictionary publication boundary and continue
to enforce MariaDB-compatible DML errors.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The tests exercise native MariaDB
view `.frm` files inside the MyLite-owned database directory, ownerless
live-peer cleanup blocking, no-live recovery, forced `.shm` rebuild, and native
exclusive reopen.

## Native Storage Impact

The base tables are InnoDB. The slice verifies base-table durability and
updatable view DML after recovery, but it does not alter InnoDB storage formats,
page-version replay policy, redo/checkpoint policy, or durable file-lifecycle
metadata.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run focused selectors:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-check-option-create-crash`
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-check-option-replace-crash`
- Run the normal embedded `view-check-option` selector.
- Run adjacent focused view crash selectors and the relevant ownerless hook SQL
  shard.
- Run DDL stress, `format-check`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- The focused selectors reach the dictionary fault hook and do not hang.
- A live peer prevents cleanup until no-live recovery.
- Create recovery exposes `CHECK_OPTION='CASCADED'`, `IS_UPDATABLE='YES'`,
  queryable view rows, valid through-view DML, and errno 1369 for invalid DML.
- Replacement recovery exposes `CHECK_OPTION='LOCAL'`, `IS_UPDATABLE='YES'`,
  the replacement predicate, valid through-view DML, and errno 1369 for invalid
  DML.
- Ownerless and ordinary native reopen observe the same state before and after
  forced `.shm` rebuild.

## Risks And Follow-Up

- This covers deterministic check-option create and replacement boundaries, not
  every updatable view semantic.
- `ALTER VIEW ... WITH CHECK OPTION` crash coverage, nested view propagation
  crash coverage, invalid dependencies, non-updatable diagnostics, prepared
  view DML, randomized view oracles, and external long-running DDL stress remain
  planned.

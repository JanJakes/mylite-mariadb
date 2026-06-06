# Ownerless View Check-Option Alter DDL Crash

## Problem Statement

Ownerless view check-option crash coverage proves creation and replacement
boundaries, but `ALTER VIEW ... WITH CHECK OPTION` remained a planned gap. That
leaves one MariaDB-native view rewrite spelling unproven at the point where
MariaDB has written the native view definition file but MyLite has not yet
published ownerless dictionary finish.

This slice adds deterministic crash-boundary evidence for check-option view
alteration.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_yacc.yy`
  - The `ALTER VIEW` grammar calls `Lex->add_alter_view(...)` and then parses
    `view_select`.
  - `view_select` consumes `view_check_option`, whose grammar accepts no
    option, `WITH CHECK OPTION`, `WITH CASCADED CHECK OPTION`, and
    `WITH LOCAL CHECK OPTION`.
- `mariadb/sql/sql_view.cc`
  - `mysql_create_view()` handles both create and alter view commands.
  - `mysql_create_view()` rejects `WITH CHECK OPTION` on a non-updatable view.
  - `mysql_register_view()` stores `view->with_check` in the native view
    definition written for the view.
- `mariadb/sql/table.cc`
  - `TABLE_LIST::view_check_option()` raises `ER_VIEW_CHECK_FAILED` when DML
    through a view violates the active predicate.
- `packages/libmylite/src/database.cc`
  - `ownerless_dictionary_ddl_statement()` classifies `ALTER` as ownerless
    dictionary DDL.
  - `ownerless_finish_dictionary_ddl()` exposes the unsafe
    `dictionary-before-finish` hook used by other DDL crash tests.

## Design

Add one unsafe-hook selector to `mylite_ownerless_cross_process_sql_test`:

- `dictionary-view-check-option-alter-crash` creates an InnoDB base table and
  an initial updatable view with `WITH LOCAL CHECK OPTION`, kills a writer after
  `ALTER VIEW ... WITH CASCADED CHECK OPTION` rewrites the native view
  definition but before ownerless dictionary finish, then verifies recovered
  `CHECK_OPTION='CASCADED'`, `IS_UPDATABLE='YES'`, the altered predicate,
  valid DML, and MariaDB errno 1369 for invalid DML.

The selector verifies ownerless and ordinary native reopen before and after a
forced `.shm` rebuild.

## Scope

In scope:

- Crash-at-`dictionary-before-finish` coverage for check-option view
  alteration.
- Recovered `.frm` presence under `datadir/app/`.
- Recovered `INFORMATION_SCHEMA.VIEWS.CHECK_OPTION` and `IS_UPDATABLE`
  metadata.
- Query behavior and valid DML through the recovered updatable view.
- Invalid insert/update failure with MariaDB errno 1369.
- Base-table writes after recovery.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Nested view local-versus-cascaded propagation crash coverage.
- Invalid dependencies, non-updatable view diagnostics, prepared statements,
  privilege/security behavior, or invalid definers.
- External randomized DDL/RQG stress.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens ownerless compatibility
evidence by proving completed MariaDB check-option view alteration survives a
writer death at MyLite's dictionary publication boundary and continues to
enforce MariaDB-compatible DML errors.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The tests exercise native MariaDB
view `.frm` files inside the MyLite-owned database directory, ownerless
live-peer cleanup blocking, no-live recovery, forced `.shm` rebuild, and native
exclusive reopen.

## Native Storage Impact

The base table is InnoDB. The slice verifies base-table durability and
updatable view DML after recovery, but it does not alter InnoDB storage formats,
page-version replay policy, redo/checkpoint policy, or durable file-lifecycle
metadata.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run the focused selector:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-check-option-alter-crash`
- Run the normal embedded `view-check-option` selector.
- Run the relevant ownerless hook SQL shard and embedded ownerless SQL shard.
- Run DDL stress, `format-check`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- A live peer prevents cleanup until no-live recovery.
- Alter recovery exposes `CHECK_OPTION='CASCADED'`, `IS_UPDATABLE='YES'`, the
  altered predicate, valid through-view DML, and errno 1369 for invalid DML.
- Ownerless and ordinary native reopen observe the same state before and after
  forced `.shm` rebuild.

## Risks And Follow-Up

- This covers deterministic check-option alteration boundaries, not every
  updatable view semantic.
- Nested view propagation crash coverage, invalid dependencies, non-updatable
  diagnostics, prepared view DML, randomized view oracles, and external
  long-running DDL stress remain planned.

# Ownerless View Security DDL Crash

## Problem Statement

Ownerless view security refresh coverage proves already-open peers observe
`DEFINER=CURRENT_USER`, `SQL SECURITY DEFINER`, and `SQL SECURITY INVOKER`
metadata changes. Hook-build crash coverage now covers simple view
create/drop, idempotent no-ops, and replacement/altered definitions, but it
does not yet prove security metadata survives a writer death after MariaDB
writes the native view definition file and before MyLite publishes ownerless
dictionary finish.

This slice adds deterministic crash-boundary evidence for creating a definer
view, replacing a definer view with an invoker view, and altering an invoker
view back to a definer view.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_parse.cc`
  - `SQLCOM_CREATE_VIEW` dispatches `CREATE VIEW`, `CREATE OR REPLACE VIEW`,
    and `ALTER VIEW` to `mysql_create_view()` using
    `thd->lex->create_view->mode`.
- `mariadb/sql/sql_view.cc`
  - `mysql_create_view()` processes `SQL SECURITY` and definer state before
    registering the view.
  - `sp_process_definer()` validates and normalizes explicit definer metadata.
  - `mysql_register_view()` stores `view->definer` and `view->view_suid` in the
    native view definition file through `sql_create_definition_file()`.
  - Successful registration removes the view from the table-definition cache
    with `tdc_remove_table()`.
- `packages/libmylite/src/database.cc`
  - Ownerless dictionary DDL classification treats `CREATE`, `ALTER`, and
    `DROP` statements as dictionary-generation boundaries, so security metadata
    rewrites use the same `dictionary-before-finish` unsafe hook as other view
    crash tests.

## Design

Add three unsafe-hook selectors to `mylite_ownerless_cross_process_sql_test`:

- `dictionary-view-security-create-crash` creates an InnoDB base table, kills a
  writer after `CREATE DEFINER=CURRENT_USER SQL SECURITY DEFINER VIEW` writes
  the native view definition but before ownerless dictionary finish, then
  verifies no-live recovery exposes `SECURITY_TYPE='DEFINER'` with non-empty
  `DEFINER` metadata.
- `dictionary-view-security-replace-crash` creates an initial definer view,
  kills a writer after `CREATE OR REPLACE SQL SECURITY INVOKER VIEW` rewrites
  the native view definition but before ownerless dictionary finish, then
  verifies no-live recovery exposes `SECURITY_TYPE='INVOKER'` with non-empty
  `DEFINER` metadata and the replacement predicate.
- `dictionary-view-security-alter-crash` creates an initial invoker view, kills
  a writer after `ALTER DEFINER=CURRENT_USER SQL SECURITY DEFINER VIEW`
  rewrites the native view definition but before ownerless dictionary finish,
  then verifies no-live recovery exposes `SECURITY_TYPE='DEFINER'` with
  non-empty `DEFINER` metadata and the altered predicate.

The selectors verify ownerless and ordinary native reopen before and after a
forced `.shm` rebuild.

## Scope

In scope:

- Crash-at-`dictionary-before-finish` coverage for explicit definer view
  creation.
- Crash-at-`dictionary-before-finish` coverage for replacement to
  `SQL SECURITY INVOKER`.
- Recovered `.frm` presence under `datadir/app/`.
- Recovered `INFORMATION_SCHEMA.VIEWS.security_type` and non-empty definer
  metadata.
- View query behavior and base-table writes after recovery.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Invalid or missing definers.
- Privilege enforcement and account lifecycle behavior.
- Stored functions, routines, and randomized view oracles.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens ownerless compatibility
evidence by proving completed MariaDB view security metadata changes survive a
writer death at MyLite's dictionary publication boundary.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The tests exercise native MariaDB
view `.frm` files inside the MyLite-owned database directory, ownerless
live-peer cleanup blocking, no-live recovery, forced `.shm` rebuild, and native
exclusive reopen.

## Native Storage Impact

The base tables are InnoDB. The slice verifies base-table durability and view
query behavior after recovery, but it does not alter InnoDB storage formats,
page-version replay policy, redo/checkpoint policy, or durable file-lifecycle
metadata.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run focused selectors:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-security-create-crash`
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-security-replace-crash`
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-security-alter-crash`
- Run the normal embedded `view-security-definer` selector.
- Run adjacent view crash selectors and the relevant ownerless hook SQL shard.
- Run DDL stress, `format-check`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- The focused selectors reach the dictionary fault hook and do not hang.
- A live peer prevents cleanup until no-live recovery.
- Create recovery exposes `SECURITY_TYPE='DEFINER'`, non-empty `DEFINER`
  metadata, the `.frm` file, and queryable view rows.
- Replacement recovery exposes `SECURITY_TYPE='INVOKER'`, non-empty `DEFINER`
  metadata, the replacement predicate, and queryable view rows.
- Alter recovery exposes `SECURITY_TYPE='DEFINER'`, non-empty `DEFINER`
  metadata, the altered predicate, and queryable view rows.
- Ownerless and ordinary native reopen observe the same state before and after
  forced `.shm` rebuild.

## Risks And Follow-Up

- This covers deterministic definer create, invoker replacement, and definer
  alter boundaries, not full security semantics.
- Invalid-definer recovery, privilege enforcement, randomized view oracles, and
  external long-running DDL stress remain planned.

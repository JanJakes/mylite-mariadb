# Ownerless View Replacement DDL Crash

## Problem Statement

Ownerless view coverage proves already-open peer refresh for
`CREATE OR REPLACE VIEW` and `ALTER VIEW`, and hook-build crash coverage proves
simple view `CREATE`/`DROP` plus idempotent view no-op boundaries. The crash
matrix still lacks deterministic evidence for native view definition rewrites:
if a writer dies after MariaDB replaces the view `.frm` file but before MyLite
publishes ownerless dictionary finish, no-live recovery must preserve the
completed replacement or alteration.

This slice adds hook-build crash selectors for `CREATE OR REPLACE VIEW` and
`ALTER VIEW` over InnoDB base tables.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_parse.cc`
  - `SQLCOM_CREATE_VIEW` dispatches both `CREATE VIEW` and `ALTER VIEW` to
    `mysql_create_view()` using `thd->lex->create_view->mode`.
- `mariadb/sql/sql_view.cc`
  - `mysql_create_view()` handles `VIEW_CREATE_NEW`, `VIEW_CREATE_OR_REPLACE`,
    and `VIEW_ALTER`.
  - `fill_defined_view_parts()` reloads existing view metadata for `ALTER
    VIEW` when omitted clauses inherit from the old definition.
  - `mysql_register_view()` checks whether the old view exists, validates it as
    a view, backs up the old definition file, and writes the new view
    definition through `sql_create_definition_file()`.
  - A successful rewrite removes the view from the table-definition cache via
    `tdc_remove_table()`.
- `packages/libmylite/src/database.cc`
  - Ownerless dictionary DDL classification treats `CREATE`, `ALTER`, and
    `DROP` statements as dictionary-generation boundaries, so replacement and
    alter view definitions reach the same `dictionary-before-finish` unsafe
    hook used by other DDL crash slices.

## Design

Add two unsafe-hook selectors to `mylite_ownerless_cross_process_sql_test`:

- `dictionary-view-replace-crash` creates an InnoDB base table and an initial
  view, kills a writer after `CREATE OR REPLACE VIEW` rewrites the native view
  definition but before ownerless dictionary finish, verifies live-peer cleanup
  remains busy, then reopens no-live ownerless and checks the replaced
  projection and column metadata.
- `dictionary-view-alter-crash` creates an InnoDB base table and an initial
  view, kills a writer after `ALTER VIEW` rewrites the native view definition
  but before ownerless dictionary finish, verifies live-peer cleanup remains
  busy, then reopens no-live ownerless and checks the altered projection and
  column metadata.

Both selectors verify ownerless and ordinary native reopen before and after a
forced `.shm` rebuild.

## Scope

In scope:

- Crash-at-`dictionary-before-finish` coverage for `CREATE OR REPLACE VIEW`.
- Crash-at-`dictionary-before-finish` coverage for `ALTER VIEW`.
- Recovered `.frm` presence under `datadir/app/`.
- Recovered `INFORMATION_SCHEMA.VIEWS` and `INFORMATION_SCHEMA.COLUMNS`
  metadata.
- Query behavior proving the old exposed `value` column is gone and the new
  projected column is active.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Invalid dependencies or invalid definer recovery.
- Check-option, nested-view, and updatable-view crash variants.
- Explicit column-list crash coverage, which is tracked separately by
  `docs/specs/ownerless-view-column-list-ddl-crash/specs.md`.
- Security/definer crash coverage, which is tracked separately by
  `docs/specs/ownerless-view-security-ddl-crash/specs.md`.
- SQL-level table-lock fault injection for native table-wait paths.
- External randomized DDL/RQG stress.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens ownerless compatibility
evidence by proving completed MariaDB view definition rewrites survive a writer
death at MyLite's dictionary publication boundary.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The tests exercise MariaDB native
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
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-replace-crash`
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-alter-crash`
- Run adjacent hook selectors for simple and idempotent view DDL crashes.
- Run normal embedded view replacement refresh coverage.
- Run the relevant ownerless hook SQL shard and DDL stress.
- Run `format-check`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- The focused selectors reach the dictionary fault hook and do not hang.
- A live peer prevents cleanup until no-live recovery.
- Replacement recovery exposes the replaced view metadata, removes the old
  `value` column, and queries the new `adjusted` projection.
- Alter recovery exposes the altered view metadata, removes the old `value`
  column, and queries the new `doubled` projection.
- Ownerless and ordinary native reopen observe the same state before and after
  forced `.shm` rebuild.

## Risks And Follow-Up

- This covers deterministic replacement and alter view rewrite boundaries, not
  every view semantic.
- Invalid dependencies, invalid definers, additional security/definer crash
  variants, updatable-view crash variants, randomized view oracles, and external
  long-running DDL stress remain planned.

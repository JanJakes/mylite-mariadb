# Ownerless View Check Option

## Problem

Ownerless view coverage proves simple view create/query/drop and definition
replacement through `CREATE OR REPLACE VIEW` and `ALTER VIEW`. It still leaves
updatable view behavior and `WITH CHECK OPTION` in the broader planned view
bucket.

`WITH CHECK OPTION` is a compatibility-sensitive view feature because the view
definition is durable metadata, but enforcement happens later during DML
through the view. Ownerless peers need evidence that an already-open handle
refreshes changed view metadata and that DML through the view enforces MariaDB's
check-option rules after each ownerless DDL boundary.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:18498` parses view check-option clauses:
  no option, `WITH CHECK OPTION`, `WITH CASCADED CHECK OPTION`, and
  `WITH LOCAL CHECK OPTION`.
- `mariadb/sql/sql_view.cc:1084` records whether a view is updatable, and
  `mariadb/sql/sql_view.cc:1216` rejects `WITH CHECK OPTION` on a non-updatable
  view.
- `mariadb/sql/sql_view.cc:853` stores the `with_check_option` view parameter in
  the native view definition file.
- `mariadb/sql/table.cc:6589` implements
  `TABLE_LIST::view_check_option()` and reports `ER_VIEW_CHECK_FAILED` when a
  row written through the view does not satisfy the view predicate.
- `mariadb/sql/sql_insert.cc:1211` checks view options during ordinary
  `INSERT`, and `mariadb/sql/sql_update.cc:1012` checks them during `UPDATE`.
- `mariadb/sql/sql_show.cc:7589` derives `INFORMATION_SCHEMA.VIEWS`
  updatability metadata when requested.
- `packages/libmylite/src/database.cc:7558` classifies view `CREATE`, `ALTER`,
  and `DROP` statements as ownerless dictionary DDL, so check-option metadata
  changes should pass through the same ownerless dictionary-generation boundary
  as the existing view DDL selectors.

## Design

Add a focused `view-check-option` selector to
`mylite_ownerless_cross_process_sql_test`.

One child ownerless process owns the view DDL:

1. Create an InnoDB base table and an updatable view with
   `WITH CASCADED CHECK OPTION`.
2. Replace the view with a narrower predicate and `WITH LOCAL CHECK OPTION`.
3. Alter the view to another predicate and `WITH CASCADED CHECK OPTION`.
4. Drop the view.

The already-open parent ownerless peer verifies each boundary:

- `INFORMATION_SCHEMA.VIEWS.CHECK_OPTION` and `IS_UPDATABLE` reflect the current
  view definition.
- `INSERT` through the view succeeds for rows satisfying the active predicate.
- `INSERT` and `UPDATE` through the view fail with MariaDB errno 1369
  (`ER_VIEW_CHECK_FAILED`) when the write would violate the active predicate.
- Base-table rows survive after the view is dropped.
- Final ownerless/native reopen checks before and after forced `.shm` rebuild
  see the view absent and the base-table rows intact.

## Scope And Non-Goals

In scope:

- Ownerless peer refresh for `WITH LOCAL CHECK OPTION` and
  `WITH CASCADED CHECK OPTION` metadata.
- DML through an updatable view from an already-open ownerless peer.
- Check-option failure mapping to MariaDB errno 1369.
- Final ownerless/native reopen checks before and after forced `.shm` rebuild.

Out of scope:

- Nested view cascaded/local distinctions beyond one base table.
- `SQL SECURITY` or definer/privilege semantics.
- Invalid dependencies, non-updatable view diagnostics, prepared statements, or
  routine interaction.
- Crash injection during view definition rewrite.

## Compatibility Impact

No product SQL semantics change. The slice expands ownerless compatibility
evidence for MariaDB updatable view check-option behavior while keeping broader
view semantics partial.

## Directory And Lifecycle Impact

No directory layout change. The test checks the native view definition file
under `datadir/app/` while the view exists and verifies it is absent after
`DROP VIEW` and no-live reopen.

## Native Storage Impact

The base table is InnoDB. The slice exercises native InnoDB DML routed through
view metadata, but it does not alter native storage formats or recovery code.

## Public API Impact

No public API change.

## Binary Size Impact

No production binary-size impact. The slice adds test code and documentation.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `view-check-option` selector in `embedded-dev`.
- Run adjacent view selectors in `embedded-dev`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run the registered ownerless cross-process SQL CTest in embedded and hook
  presets as needed for run-all coverage.
- Run `format-check`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- Already-open ownerless peers see `CASCADED` and `LOCAL` check-option metadata
  after view create/replace/alter DDL from another process.
- Valid DML through the view succeeds from the already-open peer.
- Invalid insert and update attempts fail with MariaDB errno 1369.
- View absence and base-table rows survive ownerless/native reopen before and
  after forced `.shm` rebuild.

## Risks And Follow-Up

- This does not prove nested view `LOCAL` versus `CASCADED` propagation.
- View security, invalid dependency handling, non-updatable views, prepared
  view DML, and crash recovery during view DDL remain planned broader view
  work.

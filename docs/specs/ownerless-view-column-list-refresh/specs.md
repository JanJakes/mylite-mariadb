# Ownerless View Column List Refresh

## Problem Statement

Ownerless view coverage proves simple view DDL, replacement/alteration,
idempotent behavior, check-option semantics, nested check-option propagation,
and security/definer metadata refresh. Explicit view column lists remain a
bounded view metadata variant: MariaDB stores the view's exposed column names in
native view metadata, and an already-open ownerless peer must refresh those
names when another process replaces or alters the view definition.

This slice adds deterministic SQL coverage for explicit `CREATE VIEW`,
`CREATE OR REPLACE VIEW`, and `ALTER VIEW` column lists. It does not claim
invalid dependency handling, privilege semantics, invalid column-list arity
crash cases, or randomized view-oracle coverage.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_yacc.yy`
  - `CREATE VIEW` and `ALTER VIEW` parse optional column lists before the view
    `SELECT` body.
- `mariadb/sql/sql_parse.cc`
  - `SQLCOM_CREATE_VIEW` dispatches create, replace, and alter view execution
    through `mysql_create_view()`.
  - `SQLCOM_DROP_VIEW` dispatches view removal through `mysql_drop_view()`.
- `mariadb/sql/sql_view.cc`
  - `mysql_create_view()` handles `VIEW_CREATE_NEW`, `VIEW_CREATE_OR_REPLACE`,
    and `VIEW_ALTER`.
  - `mysql_register_view()` validates and stores the view definition file,
    including explicit column aliases.
- `packages/libmylite/src/database.cc`
  - Ownerless dictionary DDL classification treats `CREATE`, `ALTER`, and
    `DROP` statements as dictionary-generation boundaries, so view column-list
    metadata changes should refresh already-open peers through the same path as
    other view DDL.

## Design

Add a focused ownerless SQL selector, `view-column-list`.

The selector starts an ownerless parent handle and a child ownerless DDL process:

1. The child creates an InnoDB base table and a view with explicit
   `(view_id, view_value, view_note)` column names.
2. The parent verifies `INFORMATION_SCHEMA.COLUMNS` names and ordinals, queries
   the explicit column names through the already-open handle, and writes a new
   base-table row.
3. The child runs `CREATE OR REPLACE VIEW` with a changed explicit column list,
   `(view_id, adjusted_value, label)`, and a narrower predicate.
4. The parent verifies the old exposed column name is gone, the replacement
   column name is visible, and the changed projection/predicate is active.
5. The child runs `ALTER VIEW` with `(view_id, doubled_value, label)`.
6. The parent verifies the second column-list change and changed projection.
7. The child drops the view. The parent verifies metadata and file absence while
   the base table remains durable.
8. Final assertions verify base-table rows and dropped-view absence through
   ownerless and ordinary exclusive reopen before and after forced `.shm`
   rebuild.

## Scope

In scope:

- Ownerless already-open peer refresh for explicit view column lists.
- Native MariaDB `INFORMATION_SCHEMA.COLUMNS` metadata visibility for view
  aliases and ordinals.
- Ownerless/native reopen and forced `.shm` rebuild checks.
- Compatibility and cross-process-concurrency documentation updates.

Out of scope:

- Invalid dependencies or invalid column-list arity.
- Privilege/security behavior.
- Invalid column-list crash recovery and invalid dependency crash recovery.
- Non-updatable views, stored routines, and randomized view oracles.

## Compatibility Impact

No intended SQL behavior change. Ownerless mode continues to inherit MariaDB
view parsing and metadata behavior while MyLite coordinates dictionary refresh
for already-open peers. Compatibility remains partial for invalid dependencies,
privilege semantics, invalid arity crash variants, and randomized view
coverage.

## Directory And Lifecycle Impact

No directory layout changes. The view definition remains MariaDB-native metadata
under `datadir/<schema>/<view>.frm` while present. Final checks verify the file
is absent after `DROP VIEW` and the base table survives ownerless/native reopen
before and after volatile shared-memory rebuild.

## Native Storage Impact

The base table is InnoDB. The selector exercises native base-table reads and
writes while view column-list metadata changes, but it does not alter InnoDB
file formats, page-version replay, redo/checkpoint policy, or DDL file
lifecycle.

## Binary Size Impact

Test and documentation only. No public API, production code path, or default
runtime feature is added.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run `build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test view-column-list`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run adjacent view selectors and the ownerless SQL CTest shard containing the
  new selector.
- Run relevant ownerless DDL stress, `format-check`, `git diff --check`, and
  cached diff checks before commit.

## Acceptance Criteria

- An already-open ownerless peer sees explicit view columns created by another
  ownerless process.
- The same peer sees replacement from `view_value` to `adjusted_value`.
- The same peer sees alteration from `adjusted_value` to `doubled_value`.
- The same peer sees final `DROP VIEW`, and final view absence plus base-table
  rows survive ownerless/native reopen before and after forced `.shm` rebuild.
- Docs cross-link the hook-build column-list crash slice while keeping invalid
  dependencies, privilege semantics, invalid arity crash injection, and
  randomized view coverage as planned.

## Risks And Follow-Up

- This proves metadata refresh for bounded explicit column-list changes, not
  invalid column-list arity or dependency failure behavior.
- Hook-build crash recovery for explicit column-list create, replace, and alter
  is covered by
  `docs/specs/ownerless-view-column-list-ddl-crash/specs.md`; invalid arity and
  dependency crash recovery remain planned.
- Invalid-dependency and randomized view-oracle coverage remain planned.

# Ownerless View DDL Variants

## Problem Statement

Ownerless view coverage currently proves simple `CREATE VIEW`, peer query, and
`DROP VIEW` refresh. The compatibility matrix still keeps broader view
semantics planned, and the existing coverage does not prove that an already-open
ownerless peer refreshes a changed view definition when another process runs
`CREATE OR REPLACE VIEW` or `ALTER VIEW`.

This slice adds bounded SQL evidence for those view-definition replacement
paths. It does not claim full view compatibility, updatable views, view security
semantics, nested views, or crash recovery during view DDL.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_parse.cc`
  - `SQLCOM_CREATE_VIEW` dispatches to `mysql_create_view()`.
  - The dispatch comment states that `SQLCOM_CREATE_VIEW` also handles
    `ALTER VIEW` through `thd->lex->create_view->mode`.
  - `SQLCOM_DROP_VIEW` dispatches to `mysql_drop_view()`.
- `mariadb/sql/sql_view.cc`
  - `mysql_create_view()` handles `VIEW_CREATE_NEW`, `VIEW_ALTER`, and
    `VIEW_CREATE_OR_REPLACE`.
  - `make_view_filename()` routes the view definition path through
    `build_table_filename()` under the configured data directory.
  - `mysql_register_view()` generates the stored view definition, validates an
    existing view for replace/alter, backs up the old definition when present,
    and writes the new definition through `sql_create_definition_file()`.
  - `mysql_drop_view()` locks view names, deletes the view definition file,
    removes the table-definition cache entry, invalidates query-cache metadata,
    and invalidates stored-program cache state.
- `packages/libmylite/src/database.cc`
  - MyLite ownerless dictionary DDL classification treats `CREATE`, `ALTER`,
    and `DROP` statements as ownerless dictionary-generation boundaries, so view
    replacement and alteration should publish through the same odd/even
    generation protocol as table DDL.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  - The existing `view-ddl` selector covers simple create/query/drop refresh
    and no-live ownerless/native reopen before and after forced `.shm` rebuild.

## Design

Add a focused selector, `view-ddl-variants`, to
`mylite_ownerless_cross_process_sql_test`.

The selector starts an ownerless parent handle and a child ownerless DDL
process:

1. The child creates an InnoDB base table and an initial view definition.
2. The parent observes the view through `INFORMATION_SCHEMA`, queries it, and
   writes a new base-table row through the already-open handle.
3. The child runs `CREATE OR REPLACE VIEW` with a narrower predicate and a
   changed projection. The parent verifies the already-open handle observes the
   replacement definition rather than the cached original definition.
4. The child runs `ALTER VIEW` with another changed projection. The parent
   verifies the already-open handle observes the altered definition.
5. The child drops the view. The parent verifies view metadata absence and the
   base table remains readable.
6. Final assertions verify base-table rows and view absence through ownerless
   and ordinary exclusive reopen before and after forced `.shm` rebuild.

## Scope

In scope:

- SQL-level ownerless coverage for `CREATE OR REPLACE VIEW` and `ALTER VIEW`.
- Already-open peer dictionary/table-definition cache refresh for changed view
  definitions.
- Compatibility and cross-process-concurrency documentation updates.

Out of scope:

- New production dictionary or storage code unless the selector reveals a bug.
- Updatable view DML, `WITH CHECK OPTION`, nested views, invalid dependencies,
  definer/security edge cases, or privilege behavior.
- Trigger or stored-routine coverage.
- Crash/fault injection during view DDL.

## Compatibility Impact

No intended SQL behavior change. The slice expands ownerless compatibility
evidence for view DDL variants while keeping broader view semantics partial.
Ordinary exclusive embedded behavior continues to inherit MariaDB behavior.

## Directory And Lifecycle Impact

No directory layout changes. The view definition remains MariaDB-native metadata
under `datadir/<schema>/<view>.frm` while present. Final checks verify that the
definition file is absent after `DROP VIEW` and that the InnoDB base table
survives ownerless/native reopen before and after volatile shared-memory
rebuild.

## Native Storage Impact

The base table is InnoDB. The selector exercises native base-table reads and
writes through view metadata refresh, but it does not change InnoDB file formats
or page-version replay semantics.

## Binary Size Impact

Test and documentation only unless a bug fix is required. No dependency,
public API, or default runtime feature is added.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `view-ddl-variants` selector in `embedded-dev`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run the relevant ownerless SQL CTest filter after implementation.
- Run `format-check`, `git diff --check`, and cached diff checks before commit.

## Acceptance Criteria

- An already-open ownerless peer sees the initial view created by another
  process.
- The same peer sees the `CREATE OR REPLACE VIEW` definition change without
  closing its handle.
- The same peer sees the `ALTER VIEW` definition change without closing its
  handle.
- The same peer sees `DROP VIEW`, and final view absence plus base-table rows
  survive ownerless/native reopen before and after forced `.shm` rebuild.
- Docs continue to mark untested view edge cases and crash recovery as planned.

## Risks And Open Questions

- MariaDB view replacement rewrites the native `.frm` definition file. This
  slice proves dictionary-generation refresh for bounded replacement shapes,
  not crash recovery if a process dies mid-rewrite.
- Broader view behavior still needs nested-view, invalid-dependency, security,
  check-option, and updatable-view coverage before MyLite can claim full view
  compatibility.

# Ownerless View DDL Refresh

## Problem

Ownerless DDL coverage now exercises table and schema metadata boundaries, but
view metadata is still grouped with broader planned view/trigger/routine work.
MariaDB views are durable schema metadata stored as definition files and opened
through the table-definition cache, so ownerless peers need evidence that
`CREATE VIEW` and `DROP VIEW` publish through the dictionary-generation protocol
and remain durable after no-live reopen.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB documentation for `CREATE VIEW`
  (`https://mariadb.com/docs/server/server-usage/views/create-view`) says a view
  belongs to a database, shares a namespace with base tables, and can refer to
  base tables or other views; the `SELECT` definition is frozen at creation
  time.
- MariaDB documentation for `DROP VIEW`
  (`https://mariadb.com/docs/server/server-usage/views/drop-view`) says it
  removes one or more views, and `DROP VIEW` for a single view is atomic in
  MariaDB 10.6.1 and later.
- `mariadb/sql/sql_parse.cc` dispatches `SQLCOM_CREATE_VIEW` to
  `mysql_create_view()` and `SQLCOM_DROP_VIEW` to `mysql_drop_view()`.
- `mariadb/sql/sql_view.cc:mysql_create_view()` rejects view DDL under
  `LOCK TABLES`, resolves and locks referenced tables, and sets an exclusive
  metadata-lock request for the target view when needed.
- `mariadb/sql/sql_view.cc:make_view_filename()` builds the view definition
  path with `build_table_filename()` under the configured data directory.
- `mariadb/sql/sql_view.cc:mysql_register_view()` writes the view definition
  through `sql_create_definition_file()` after recording the view type and
  metadata parameters.
- `mariadb/sql/sql_view.cc:mysql_drop_view()` locks the target view names,
  deletes the view definition file, removes the table-definition cache entry,
  invalidates query-cache metadata, and invalidates stored-program cache state.
- MyLite ownerless DDL classification in `packages/libmylite/src/database.cc`
  treats `CREATE` and `DROP` statements as dictionary DDL, so view creation and
  deletion publish through the same ownerless odd/even dictionary-generation
  protocol used by table and schema DDL.

## Scope And Non-Goals

- Add a focused ownerless SQL selector for view create/query/drop metadata.
- Verify an already-open ownerless peer observes a view created by another
  ownerless process, can query it over an InnoDB base table, and can mutate the
  base table with the view reflecting the new committed rows.
- Verify the same peer observes `DROP VIEW` and the final state survives
  ownerless/native reopen before and after forced `.shm` rebuild.
- Do not add trigger, stored-function, prepared `CALL`, or broader routine
  coverage.
- Do not add SQL-level table-lock fault injection; prior exploratory SQL shapes
  did not reach the ownerless table-wait callback.
- Do not claim full view compatibility, view security semantics, updatable
  views, or crash recovery during view DDL.

## Design

- Add `view-ddl` to `mylite_ownerless_cross_process_sql_test`.
- The child ownerless process creates an InnoDB base table, inserts one row,
  creates a simple view over that table, then signals the parent.
- The parent keeps an ownerless handle open, observes the view through
  `information_schema`, queries the view, inserts a second base-table row, and
  verifies the view result reflects both rows.
- The child drops the view. The parent observes view metadata absence from the
  same already-open handle and verifies selecting from the dropped view fails
  while the base table remains readable.
- After both ownerless handles close, helper assertions verify base-table rows,
  view absence, and removed view definition file through:
  - `MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW`,
  - `MYLITE_OPEN_READWRITE`,
  - forced `concurrency/mylite-concurrency.shm` deletion plus ownerless reopen,
  - ordinary exclusive read/write reopen after the forced rebuild.

## Compatibility Impact

No SQL behavior changes. The compatibility matrix gains ownerless evidence for
simple view create/query/drop dictionary refresh and no-live reopen while
keeping broader views, triggers, and routines partial.

## Directory And Lifecycle Impact

The slice exercises MariaDB native view metadata inside the MyLite-owned
database directory: `datadir/app/ownerless_view.frm` while the view exists, and
absence of that file after `DROP VIEW` and no-live reopen. It also verifies the
underlying InnoDB base table remains durable through ownerless and ordinary
exclusive reopen before and after volatile shared-memory recreation.

## Native Storage Impact

No native storage format changes. The base table is InnoDB so view queries also
exercise ownerless native-file/page-version refresh over durable rows while
view metadata changes publish through the dictionary-generation protocol.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `view-ddl` selector.
- Build and run the focused `view-ddl` selector in `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL coverage or a focused rerun if the
  known intermittent InnoDB log-header checksum abort appears.
- Run the ownerless stress preset, `format-check`, `git diff --check`, and
  cached diff checks before commit.

## Acceptance Criteria

- Already-open ownerless peers see a view created by another ownerless process.
- The peer can query through the view and see base-table changes committed from
  either ownerless process.
- Already-open peers see the view disappear after `DROP VIEW`, while the base
  table remains readable.
- Final view absence and base-table state survive ownerless/native reopen before
  and after forced `.shm` rebuild.
- Compatibility docs keep broader view/trigger/routine and external-oracle
  stress gaps marked partial/planned.

## Risks And Follow-Up

- View security, updatable views, nested views, invalid dependency handling,
  triggers, stored functions, and prepared routine calls remain outside this
  slice.
- Crash recovery during view DDL and broader DDL-created file lifecycle
  recovery remain planned.

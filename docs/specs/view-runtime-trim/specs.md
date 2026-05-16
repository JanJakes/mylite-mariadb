# View Runtime Trim

## Problem

MyLite rejects persistent views before MariaDB execution because view
definitions are non-table database objects that need catalog-backed metadata
before they can fit the single-file runtime. The default embedded profile still
links MariaDB's file-backed view DDL, `.frm` view definition loader, view
rename/repair helpers, and view expansion runtime.

That retained runtime is not useful while persistent views are unsupported. It
also keeps filesystem-backed view metadata close to the default embedded
profile, where MyLite must not publish durable MariaDB sidecars.

## Source Findings

Base: MariaDB 11.8.6,
`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.

- MariaDB documents `CREATE VIEW` as creating a virtual table over a stored
  `SELECT` definition, with `OR REPLACE`, `ALGORITHM`, `DEFINER`,
  `SQL SECURITY`, and `WITH CHECK OPTION` semantics:
  <https://mariadb.com/docs/server/server-usage/views/create-view>.
- MariaDB documents view processing algorithms as `MERGE`, `TEMPTABLE`, and
  `UNDEFINED`; those algorithms affect query planning, updatability, and lock
  behavior:
  <https://mariadb.com/docs/server/server-usage/views/view-algorithms>.
- `mariadb/sql/sql_yacc.yy` parses view DDL into `SQLCOM_CREATE_VIEW` and
  `SQLCOM_DROP_VIEW`, and `SHOW CREATE VIEW` uses the normal SHOW CREATE path
  with a view table type.
- `mariadb/sql/sql_parse.cc` dispatches view DDL through
  `mysql_create_view()` and `mysql_drop_view()`.
- `mariadb/sql/sql_view.cc` mixes two categories of functionality:
  persistent view DDL/loading/maintenance (`mysql_create_view()`,
  `mysql_make_view()`, `mysql_drop_view()`, `mysql_rename_view()`,
  `view_check()`, and `view_repair()`) and general SELECT-list helper logic
  (`check_duplicate_names()` and `make_valid_column_names()`) used by derived
  tables and CTEs.
- `mariadb/sql/sql_base.cc` calls `mysql_make_view()` when table-share
  discovery finds a view. `mariadb/sql/sql_show.cc` also calls it while
  populating view-related metadata.
- `mariadb/sql/sql_insert.cc`, `mariadb/sql/sql_update.cc`, and
  `mariadb/sql/sql_delete.cc` call `check_key_in_view()` and
  `insert_view_fields()` for updatable view paths.
- `mariadb/sql/sql_admin.cc` and `mariadb/sql/sql_rename.cc` call view
  checksum/repair and rename helpers for file-backed server view metadata.
- `mariadb/sql/sql_cte.cc` and `mariadb/sql/sql_union.cc` call
  `check_duplicate_names()`, and `mariadb/sql/sql_cte.cc` calls
  `make_valid_column_names()`. Those helpers must remain available because
  MyLite still supports derived-table and CTE-shaped SELECT behavior through
  the retained MariaDB parser and optimizer.
- The current default embedded archive retains `sql_view.cc.o`
  (26,552 bytes on 2026-05-16) even though MyLite rejects representative view
  DDL before MariaDB execution.

## Design

- Add `MYLITE_WITH_VIEW_RUNTIME`, defaulting to `ON` for upstream-style builds
  and forced `OFF` in `cmake/mariadb-embedded-baseline.cmake`.
- When enabled, build MariaDB's existing `sql_view.cc` unchanged.
- When disabled, build a MyLite view-runtime stub instead of `sql_view.cc`.
- Preserve the helper functions that are not persistent-view runtime:
  `check_duplicate_names()` and `make_valid_column_names()` keep the MariaDB
  semantics needed by derived tables, CTEs, and `CREATE TABLE ... SELECT`
  projection naming.
- The disabled stub keeps the link-visible view API stable:
  `create_view_precheck()`, `mysql_create_view()`, `mysql_make_view()`,
  `mysql_drop_view()`, `check_key_in_view()`, `insert_view_fields()`,
  `view_checksum()`, `view_check()`, `view_repair()`,
  `mysql_rename_view()`, `mariadb_view_version_get()`, and `view_type`
  remain defined.
- The disabled stub must fail closed for persistent view DDL, file-backed view
  loading, and direct view maintenance paths by reporting an unsupported view
  runtime and returning failure.
- Non-view table DML paths that call view helpers must continue to behave as
  ordinary table paths. `check_key_in_view()` and `insert_view_fields()` return
  success for normal tables with no view translation state.
- Keep direct and prepared MyLite SQL policy rejecting `CREATE VIEW`,
  `DROP VIEW`, and `SHOW CREATE VIEW` before MariaDB execution.
- Keep `INFORMATION_SCHEMA.VIEWS` visible, but with no rows in the disabled
  embedded profile.
- Do not implement catalog-backed persistent views in this slice.

## Affected Subsystems

- MariaDB embedded build profile and `libmysqld` source selection.
- File-backed view DDL, loader, rename, checksum, and repair helpers.
- Derived-table, CTE, and projection naming helpers retained from `sql_view.cc`.
- Public direct/prepared SQL tests for unsupported non-table objects and
  server-surface metadata.
- Compatibility matrix, embedded-build documentation, roadmap, and
  compatibility harness labels.

## Compatibility Impact

MariaDB Server supports persistent views, view algorithms, view security
contexts, updatable views, `SHOW CREATE VIEW`, and
`INFORMATION_SCHEMA.VIEWS`. MyLite's default embedded profile intentionally
does not support persistent view creation, view expansion from `.frm` metadata,
or filesystem-backed view metadata.

This does not change the current user-visible MyLite policy for view DDL:
view DDL and `SHOW CREATE VIEW` remain rejected before MariaDB execution. The
intended metadata behavior is that `INFORMATION_SCHEMA.VIEWS` remains queryable
with zero rows because no view metadata is published.

## DDL Metadata Routing Impact

No supported table DDL changes. The disabled profile removes MariaDB's
filesystem-backed view sidecar path while preserving base-table DDL and
derived/CTE projection naming. Catalog-backed persistent views remain planned
after base table DDL, row storage, and transaction semantics are stable.

## Single-File And Embedded-Lifecycle Impact

No file-format change and no new companion files. The disabled profile prevents
the default embedded runtime from loading, writing, renaming, repairing, or
removing MariaDB view `.frm` metadata files.

## Public API And File-Format Impact

No C API or `.mylite` format change. Existing direct and prepared statement
rejection paths continue to expose stable MyLite diagnostics for unsupported
view SQL.

## Storage-Engine Routing Impact

No handler routing change. Routed tables continue to behave as base tables.
View-backed row reads and updatable views remain unsupported until MyLite has
catalog-backed view definitions, dependency tracking, expansion semantics, and
transaction-aware metadata.

## Wire-Protocol Or Integration-Package Impact

Future wire-protocol or adapter layers should inherit the same view policy from
the core library: persistent view DDL is unsupported, view metadata is empty,
and ordinary routed base-table operations do not expand stored view
definitions.

## Binary-Size Impact

The bundle-size research ranked view runtime trimming as a small, clean
server-surface reduction when the derived-table and CTE helpers remain
available. The pre-slice default archive kept `sql_view.cc.o` as a
26,552-byte object.

Measured on 2026-05-16:

| Profile | Archive | Size | Members | Delta From Previous Baseline |
| --- | --- | ---: | ---: | ---: |
| Default embedded | `build/mariadb-embedded/libmysqld/libmariadbd.a` | 26,842,256 bytes / 25.60 MiB | 670 | -22,576 bytes, same members |
| Storage-smoke | `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a` | 27,022,840 bytes / 25.77 MiB | 673 | -22,568 bytes, same members |

## License And Dependency Impact

No new dependency and no license change. The disabled profile substitutes a
small GPL-2.0-compatible MyLite stub for retained MariaDB GPL-2.0 view runtime
code while preserving the general SELECT-list helper behavior needed by retained
SQL semantics.

## Test And Verification Plan

- Keep direct and prepared SQL tests proving representative `CREATE VIEW`,
  `DROP VIEW`, and `SHOW CREATE VIEW` surfaces are rejected before MariaDB
  execution.
- Add direct and prepared SQL tests proving `INFORMATION_SCHEMA.VIEWS` remains
  queryable with zero rows.
- Run default and storage-smoke MariaDB embedded builds and measurements.
- Run `embedded-dev`, `storage-smoke-dev`, and `dev` build/test presets.
- Run the server-surface compatibility harness, size report, formatting, tidy,
  shell syntax checks, and diff checks.

## Acceptance Criteria

- The disabled embedded profile compiles with `MYLITE_WITH_VIEW_RUNTIME=OFF`.
- View DDL and `SHOW CREATE VIEW` remain rejected before MariaDB execution with
  stable MyLite diagnostics.
- `INFORMATION_SCHEMA.VIEWS` remains visible with zero rows.
- Derived-table, CTE, CTAS, and projection duplicate-name tests continue to
  pass through the retained helper behavior.
- Ordinary routed table CREATE, DROP, RENAME, ALTER, INSERT, UPDATE, DELETE,
  REPLACE, and storage-engine smoke tests continue to pass.
- No MariaDB view `.frm` sidecars are introduced by covered MyLite table
  workflows.
- Size measurements and compatibility documentation are updated.

## Risks

- `sql_view.cc` is mixed-purpose. Replacing the whole source file is safe only
  if the stub preserves the non-view helper functions used by derived tables,
  CTEs, and CTAS projection naming.
- View helpers are referenced from insert, update, delete, admin, rename,
  information-schema, and table-open paths. The stub must preserve MariaDB
  success/error return conventions for normal base tables.
- Applications that require views remain unsupported until MyLite designs
  catalog-backed view definitions and dependency-aware invalidation.

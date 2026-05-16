# Trigger Runtime Trim

## Problem

MyLite rejects persistent triggers before MariaDB execution because trigger
metadata and trigger bodies are non-table database objects that need a
catalog-backed design before they can fit the single-file runtime. The default
embedded profile still links MariaDB's file-backed trigger loader, `.TRG` /
`.TRN` metadata maintenance, trigger body runtime dispatch, and table-rename
trigger-file update code.

That retained runtime is not useful while triggers are unsupported. It also
keeps filesystem-backed trigger behavior close to the core embedded profile,
where MyLite must not publish durable metadata sidecars.

## Source Findings

Base: MariaDB 11.8.6,
`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.

- MariaDB documents `CREATE TRIGGER` as creating a named object associated
  with a permanent table and activated by `INSERT`, `UPDATE`, or `DELETE`:
  <https://mariadb.com/kb/en/create-trigger/>.
- MariaDB documents trigger execution timing and event behavior, including
  `BEFORE` / `AFTER` variants and `REPLACE` / `ON DUPLICATE KEY UPDATE`
  trigger sequences:
  <https://mariadb.com/docs/server/server-usage/triggers-events/triggers/trigger-overview>.
- `mariadb/sql/sql_yacc.yy` parses trigger DDL into `SQLCOM_CREATE_TRIGGER`,
  `SQLCOM_DROP_TRIGGER`, and `SQLCOM_SHOW_CREATE_TRIGGER`.
- `mariadb/sql/sql_parse.cc` dispatches trigger DDL through
  `mysql_create_or_drop_trigger()` and loads trigger table names with
  `add_table_for_trigger()` when dropping triggers.
- `mariadb/sql/sql_trigger.cc` implements the file-backed trigger subsystem:
  `.TRG` / `.TRN` path construction, trigger-file parsing, trigger-file
  writing/removal, table-rename trigger metadata updates, and
  `Table_triggers_list::process_triggers()` dispatch into stored-program
  trigger bodies.
- `mariadb/sql/sql_base.cc` calls `Table_triggers_list::check_n_load()` while
  opening table shares so trigger metadata can attach to `TABLE::triggers`.
- `mariadb/sql/sql_show.cc` uses `Table_triggers_list::check_n_load()` for
  `INFORMATION_SCHEMA.TRIGGERS` and `load_table_name_for_trigger()` /
  table-opening logic for `SHOW CREATE TRIGGER`.
- `mariadb/sql/sql_table.cc`, `mariadb/sql/sql_rename.cc`, and
  `mariadb/sql/ddl_log.cc` call trigger rename/drop helpers during table DDL so
  `.TRG` and `.TRN` files stay aligned with table names in the server
  datadir-backed model.
- The current default embedded archive retains `sql_trigger.cc.o`
  (49,080 bytes on 2026-05-16) even though MyLite rejects representative
  trigger DDL before MariaDB execution.

## Design

- Add `MYLITE_WITH_TRIGGER_RUNTIME`, defaulting to `ON` for upstream-style
  builds and forced `OFF` in `cmake/mariadb-embedded-baseline.cmake`.
- When enabled, build MariaDB's existing `sql_trigger.cc` unchanged.
- When disabled, build a MyLite trigger-runtime stub instead of
  `sql_trigger.cc`.
- The disabled stub keeps the link-visible trigger API stable:
  `mysql_create_or_drop_trigger()`, `add_table_for_trigger()`,
  `load_table_name_for_trigger()`, `Table_triggers_list` methods, trigger
  rename/drop helpers, and file-removal hooks remain defined.
- The disabled stub must fail closed for trigger DDL or direct trigger lookup
  paths by reporting an unsupported trigger runtime or trigger-does-not-exist
  error and returning failure.
- The disabled stub must make ordinary table opens, DML, table DROP, table
  RENAME, copy ALTER, and recovery replay operate as "no triggers present":
  `Table_triggers_list::check_n_load()` returns success without attaching a
  trigger list, rename/drop helpers are no-ops, and trigger execution helpers
  return success when called on an empty list.
- Keep direct and prepared MyLite SQL policy rejecting `CREATE TRIGGER`,
  `DROP TRIGGER`, and `SHOW CREATE TRIGGER` before MariaDB execution.
- Keep the `INFORMATION_SCHEMA.TRIGGERS` table definition visible, but with no
  rows in the disabled embedded profile.
- Do not change view handling in this slice. `sql_view.cc` contains persistent
  view DDL/loader code but also derived-table and CTE helpers that need a
  separate source audit.

## Affected Subsystems

- MariaDB embedded build profile and `libmysqld` source selection.
- Trigger loader/runtime and trigger metadata helpers.
- Table open, table DDL, rename, and recovery paths that currently call trigger
  helpers.
- Public direct/prepared SQL tests for unsupported non-table objects and
  server-surface metadata.
- Compatibility matrix, embedded-build documentation, roadmap, and compatibility
  harness labels.

## Compatibility Impact

MariaDB Server supports persistent triggers and exposes trigger metadata through
`SHOW CREATE TRIGGER`, `SHOW TRIGGERS`, and `INFORMATION_SCHEMA.TRIGGERS`.
MyLite's default embedded profile intentionally does not support persistent
trigger creation, execution, or filesystem-backed trigger metadata.

This does not change the current user-visible MyLite policy for trigger DDL:
trigger DDL and `SHOW CREATE TRIGGER` remain rejected before MariaDB execution.
The intended metadata behavior is that `INFORMATION_SCHEMA.TRIGGERS` remains
queryable with zero rows because no trigger metadata is published.

## DDL Metadata Routing Impact

No supported table DDL changes. The disabled profile removes MariaDB's
filesystem-backed trigger sidecar path while preserving table DDL behavior as if
no triggers exist. Catalog-backed trigger metadata remains planned after base
table DDL, row storage, and transaction semantics are stable.

## Single-File And Embedded-Lifecycle Impact

No file-format change and no new companion files. The disabled profile prevents
the default embedded runtime from loading, writing, renaming, or removing
MariaDB `.TRG` and `.TRN` trigger metadata files.

## Public API And File-Format Impact

No C API or `.mylite` format change. Existing direct and prepared statement
rejection paths continue to expose stable MyLite diagnostics for unsupported
trigger SQL.

## Storage-Engine Routing Impact

No handler routing change. Routed tables continue to behave as tables without
triggers. Trigger-driven row mutations remain unsupported until MyLite has
catalog-backed trigger definitions, trigger execution semantics, statement
rollback integration, and write-concurrency behavior.

## Wire-Protocol Or Integration-Package Impact

Future wire-protocol or adapter layers should inherit the same trigger policy
from the core library: trigger DDL is unsupported, trigger metadata is empty,
and ordinary routed table DML does not invoke trigger bodies.

## Binary-Size Impact

The bundle-size research ranked trigger runtime trimming as a small but clean
server-surface reduction. The pre-slice default archive kept `sql_trigger.cc.o`
as a 49,080-byte object.

Measured on 2026-05-16:

| Profile | Archive | Size | Members | Delta From Previous Baseline |
| --- | --- | ---: | ---: | ---: |
| Default embedded | `build/mariadb-embedded/libmysqld/libmariadbd.a` | 26,864,832 bytes / 25.62 MiB | 670 | -37,712 bytes, same members |
| Storage-smoke | `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a` | 27,045,408 bytes / 25.79 MiB | 673 | -37,720 bytes, same members |

## License And Dependency Impact

No new dependency and no license change. The slice only substitutes a small
first-party GPL-2.0-compatible MyLite stub for retained MariaDB GPL-2.0 trigger
runtime code in the disabled embedded profile.

## Test And Verification Plan

- Keep direct SQL tests proving representative `CREATE TRIGGER`,
  `DROP TRIGGER`, and `SHOW CREATE TRIGGER` surfaces are rejected before MariaDB
  execution.
- Add direct and prepared SQL tests proving `INFORMATION_SCHEMA.TRIGGERS`
  remains queryable with zero rows.
- Run default and storage-smoke MariaDB embedded builds and measurements.
- Run `embedded-dev`, `storage-smoke-dev`, and `dev` build/test presets.
- Run the server-surface compatibility harness, size report, formatting, tidy,
  shell syntax checks, and diff checks.

## Acceptance Criteria

- The disabled embedded profile compiles with
  `MYLITE_WITH_TRIGGER_RUNTIME=OFF`.
- Trigger DDL and `SHOW CREATE TRIGGER` remain rejected before MariaDB
  execution with stable MyLite diagnostics.
- `INFORMATION_SCHEMA.TRIGGERS` remains visible with zero rows.
- Ordinary routed table CREATE, DROP, RENAME, ALTER, INSERT, UPDATE, DELETE,
  REPLACE, and storage-engine smoke tests continue to pass.
- No MariaDB `.TRG` or `.TRN` trigger metadata sidecars are introduced by
  covered MyLite table workflows.
- Size measurements and compatibility documentation are updated.

## Risks

- `sql_trigger.cc` exposes many small helper symbols that are referenced from
  table open, DDL, replication, and SHOW paths. The stub must preserve those
  symbols with correct MariaDB success/error return conventions.
- Trigger metadata is part of MariaDB's table DDL lifecycle. No-oping trigger
  rename/drop helpers is correct only because MyLite rejects trigger creation
  and does not import `.TRG` / `.TRN` files into the default embedded runtime.
- Applications that require triggers remain unsupported until MyLite designs
  catalog-backed triggers and transaction-aware trigger execution.

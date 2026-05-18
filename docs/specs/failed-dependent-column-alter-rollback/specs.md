# Failed Dependent Column Alter Rollback

## Goal

Cover failed `ALTER TABLE ... DROP COLUMN` attempts where the target column is
still referenced by a generated column. MyLite already covers several failed
copy-ALTER rollback paths; this slice covers dependency validation failures
that must leave the original table definition, rows, generated metadata, and
index metadata intact.

## Non-Goals

- Do not add general transactional DDL.
- Do not cover every generated dependency shape.
- Do not change generated-column expression semantics.
- Do not change the MyLite public API, file format, or size profile.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc:mysql_prepare_alter_table()` validates column
  changes and dependency metadata before a supported copy ALTER can publish a
  new definition.
- `mariadb/sql/unireg.cc:pack_vcols()` stores generated-column expressions in
  the table-definition image.
- `mariadb/sql/table.cc:parse_vcol_defs()` restores generated-column metadata
  from the stored table-definition image during reopen.
- `packages/libmylite/src/database.cc:mylite_exec()` wraps file-backed direct
  SQL in statement checkpoints so failed file-backed statements preserve the
  previous visible state.

## Compatibility Impact

This narrows the broader SQL rollback gap for supported generated-column
metadata. It verifies that MariaDB dependency rejection does not leave a
partially changed MyLite catalog or stale generated/index state.

## Design

Add storage-engine smoke coverage that:

1. creates a routed table with a generated column depending on `title`,
2. adds an index over the generated column,
3. verifies dropping `title` fails and generated values/indexes remain usable,
   and
4. repeats key checks after close/reopen.

## File Lifecycle

The failed ALTERs must leave all durable state in the primary `.mylite` file
and must not publish a partial rebuilt definition or sidecar files.

## Embedded Lifecycle And API

No public API change is required. The behavior is visible through ordinary
direct SQL execution.

## Storage-Engine Routing

The table uses explicit `ENGINE=InnoDB`, which routes to MyLite while
preserving the requested engine name.

## Build, Size, And Dependencies

No dependency or intended size-profile change is introduced.

## Test Plan

- Add storage-engine smoke coverage for failed dependent column drops.
- Update compatibility, storage architecture, roadmap, and generated-column
  coverage docs.
- Build `mylite_embedded_storage_engine_test`.
- Run the focused storage-engine smoke binary.
- Run `ctest --preset storage-smoke-dev`, `ctest --preset dev`, and
  `git diff --check`.

## Acceptance Criteria

- Dropping a base column used by a generated column fails.
- The original generated values, generated-column index, rows, and catalog
  metadata remain usable before and after reopen.
- No durable sidecars are created.

## Risks And Unresolved Questions

- This remains representative rollback coverage. Broader dependency graphs and
  full transactional DDL remain planned.

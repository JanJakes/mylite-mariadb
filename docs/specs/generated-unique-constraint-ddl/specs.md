# Generated Unique Constraint DDL

## Goal

Cover `ALTER TABLE ... ADD CONSTRAINT ... UNIQUE` and
`DROP CONSTRAINT` for a generated column on a MyLite-routed table. Existing
coverage proves ordinary generated-column unique indexes and non-CHECK unique
constraints independently; this slice covers the combined SQL surface.

## Non-Goals

- Do not add MySQL-style expression indexes.
- Do not support generated primary keys; MariaDB rejects them and MyLite
  already preserves that policy.
- Do not add online or in-place ALTER support.
- Do not claim exhaustive non-CHECK constraint syntax coverage.
- Do not change the MyLite public API or file format.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy` parses `ALTER TABLE ... ADD CONSTRAINT ... UNIQUE`
  into MariaDB key metadata through the same path used by ordinary unique keys.
- `mariadb/sql/sql_table.cc` resolves `DROP CONSTRAINT` to `DROP KEY` when the
  name matches a unique key.
- `mariadb/sql/table.cc:TABLE::update_virtual_fields()` computes generated
  values before write and read paths.
- `mariadb/storage/mylite/ha_mylite.cc` stores MariaDB table-definition images
  in the MyLite catalog and maintains supported unique index entries through
  insert, update, delete, copy ALTER, and reopen.

## Compatibility Impact

This narrows the broader non-CHECK constraint matrix gap for routed MyLite
tables and extends generated-column index coverage to named unique-constraint
syntax. Generated columns and non-CHECK constraints remain partial because
exhaustive syntax, unsupported key classes, and rollback matrices remain
planned.

## Design

Add storage-engine smoke coverage that:

1. creates a routed table with a stored generated slug column,
2. inserts distinct base rows,
3. adds `CONSTRAINT generated_slug_unique UNIQUE (slug)` through copy ALTER,
4. verifies duplicate generated values fail and forced-index reads work,
5. closes and reopens the database and repeats the key assertions,
6. drops the constraint with `DROP CONSTRAINT`, and
7. verifies the generated column remains readable while duplicate generated
   values can be inserted after the unique key is gone.

## File Lifecycle

No file-format or companion-file change is required. The generated expression,
constraint-backed key metadata, rows, and index entries remain in the primary
`.mylite` file.

## Embedded Lifecycle And API

No public `libmylite` API change is required. The behavior is visible through
ordinary SQL execution and existing diagnostics.

## Storage-Engine Routing

The test uses explicit `ENGINE=InnoDB`, which routes to MyLite while preserving
the requested engine name.

## Build, Size, And Dependencies

No dependency or intended size-profile change is introduced.

## Test Plan

- Add storage-engine smoke coverage for generated-column unique constraint
  add, duplicate enforcement, forced-index reads, close/reopen, drop, and
  post-drop duplicate inserts.
- Update compatibility, storage architecture, roadmap, and adjacent specs.
- Build `mylite_embedded_storage_engine_test`.
- Run the focused storage-engine smoke binary.
- Run `ctest --preset storage-smoke-dev`, `ctest --preset dev`, and
  `git diff --check`.

## Acceptance Criteria

- A named unique constraint can be added to a supported generated column.
- The generated unique constraint rejects duplicate generated values and
  supports forced-index reads before and after reopen.
- Dropping the constraint removes the unique key without removing the generated
  column.
- Duplicate generated values are accepted after the constraint is dropped.
- Docs keep broader non-CHECK constraint matrices marked as planned.

## Risks And Unresolved Questions

- Composite constraints over virtual generated columns are covered by the
  separate `generated-unique-constraint-matrix` slice; BLOB/TEXT generated
  columns remain a separate matrix item.
- Failed add rollback for duplicate existing generated values is covered by the
  separate `failed-generated-unique-constraint-rollback` slice.

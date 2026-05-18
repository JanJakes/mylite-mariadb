# Failed Generated Unique Constraint Rollback

## Goal

Cover failed `ALTER TABLE ... ADD CONSTRAINT ... UNIQUE` over a generated
column when existing generated values contain duplicates. MyLite already covers
failed base-column unique adds and successful generated-column unique
constraints; this slice proves the generated-column failure path preserves the
old table.

## Non-Goals

- Do not add full transactional DDL semantics.
- Do not cover virtual generated columns, composite generated constraints, or
  BLOB/TEXT generated columns.
- Do not change generated primary-key policy.
- Do not change the MyLite public API or file format.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc` routes supported `ALTER TABLE ... ADD
  CONSTRAINT ... UNIQUE` through the copy-ALTER rebuild path.
- `mariadb/sql/table.cc:TABLE::update_virtual_fields()` computes generated
  column values while rows are copied into the rebuilt table.
- `mariadb/sql/key.cc:key_copy()` builds unique-key tuples from the generated
  row buffer.
- `packages/libmylite/src/database.cc:mylite_exec()` wraps direct file-backed
  ALTER statements in a storage checkpoint and rolls that checkpoint back when
  MariaDB reports an error.
- `mariadb/storage/mylite/ha_mylite.cc:mylite_begin_statement_checkpoint()`
  reuses the existing outer checkpoint for handler work inside the statement.

## Compatibility Impact

This narrows the broader SQL rollback and non-CHECK constraint matrix gaps for
generated columns. It does not claim durable transactional DDL; it proves the
current direct-statement checkpoint protects the old routed table for this
representative generated unique-key failure.

## Design

Add storage-engine smoke coverage that:

1. creates a routed table with a stored generated slug column,
2. inserts two rows whose generated slug values collide,
3. attempts to add a named unique constraint over the generated column,
4. verifies the ALTER fails and no generated unique index is visible,
5. verifies both original rows and generated values remain visible,
6. updates one row to remove the duplicate generated value,
7. successfully adds the generated unique constraint, and
8. verifies duplicate enforcement and close/reopen visibility.

## File Lifecycle

The failed copy-ALTER must leave durable state in the primary `.mylite` file
only and must not publish the failed rebuilt table definition or generated
index entries.

## Embedded Lifecycle And API

No public `libmylite` API change is required. The behavior is visible through
ordinary direct SQL execution.

## Storage-Engine Routing

The table uses explicit `ENGINE=InnoDB`, which routes to MyLite while
preserving the requested engine name.

## Build, Size, And Dependencies

No dependency or intended size-profile change is introduced.

## Test Plan

- Add storage-engine smoke coverage for failed generated unique constraint add
  rollback and later successful add.
- Update compatibility, storage architecture, roadmap, and adjacent specs.
- Build `mylite_embedded_storage_engine_test`.
- Run the focused storage-engine smoke binary.
- Run `ctest --preset storage-smoke-dev`, `ctest --preset dev`, and
  `git diff --check`.

## Acceptance Criteria

- Failed generated unique constraint add leaves the original rows and generated
  values visible.
- The failed generated unique index is absent after the failed ALTER.
- A later successful add enforces duplicates and survives reopen.
- Docs keep broader SQL rollback and exhaustive constraint matrices planned.

## Risks And Unresolved Questions

- This only covers direct file-backed SQL with the current statement
  checkpoint. Durable transactional DDL and savepoint semantics remain planned.

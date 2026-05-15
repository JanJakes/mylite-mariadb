# Constraint And Generated Dump Import

## Problem

CHECK and generated-column coverage now includes direct DDL, ALTER, CTAS,
prepared diagnostics, and close/reopen behavior. The remaining practical gap is
importing those definitions from SQL dump-style fixtures, where applications and
migration tools replay `CREATE TABLE` and `INSERT` statements instead of
issuing them one by one in handwritten tests.

This slice adds representative import evidence without claiming full
`mysqldump` option compatibility or SQL dump export.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc` creates routed `ENGINE=InnoDB` tables through the
  MyLite handler in the storage-smoke profile.
- `mariadb/sql/unireg.cc` packs CHECK and generated-column definitions into
  the table-definition image.
- `mariadb/sql/table.cc` unpacks CHECK and generated-column metadata on reopen
  and evaluates CHECK constraints through `TABLE::verify_constraints()`.
- `mariadb/sql/sql_insert.cc` evaluates generated columns before handler writes
  and routes inserted rows through the normal handler path.
- `packages/libmylite/tests/embedded_storage_engine_test.c:exec_sql_fixture()`
  replays semicolon-delimited fixture statements while preserving quoted string
  contents and line comments.

## Design

Add a small SQL fixture under `packages/libmylite/tests/fixtures/` that looks
like an application dump fragment:

- one routed `ENGINE=InnoDB` table,
- named CHECK constraints,
- virtual and stored generated columns,
- a unique index on a stored generated column,
- a secondary index on a virtual generated column, and
- seed rows inserted through ordinary `INSERT` statements.

Add storage-engine smoke coverage that imports the fixture into a named schema,
then verifies:

- MyLite catalog metadata was published,
- generated values are readable,
- forced generated-index reads work,
- CHECK failures still reject bad rows,
- generated unique-key failures retain prepared diagnostics, and
- the imported definition and rows still work after close/reopen.

## Supported Scope

- Representative SQL fixture import for CHECK and generated-column table
  definitions.
- Inserted fixture rows on supported routed table shapes.
- Close/reopen discovery of imported CHECK and generated-column metadata.
- Constraint and generated-index behavior after import.

## Non-Goals

- Full `mysqldump` directive compatibility, including versioned comments,
  locks, triggers, routines, users, character-set toggles, or replication
  metadata.
- SQL dump export.
- Broad CHECK/generated expression matrices.
- Transaction rollback or savepoint behavior while importing a dump.
- Foreign-key-enabled dump import.

## Compatibility Impact

CHECK and generated-column support remain partial, but representative
dump-style import is now covered for supported routed table shapes. Broader
dump/export compatibility and expression matrices remain planned.

## DDL Metadata Routing Impact

Imported `CREATE TABLE` statements publish normal MyLite catalog table
definitions. The MariaDB table-definition image carries CHECK and generated
metadata; no separate MyLite constraint catalog is added.

## Single-File And Embedded-Lifecycle Impact

The imported table definition, rows, and index entries live in the primary
`.mylite` file. No durable MariaDB sidecar is introduced, and existing sidecar
gates prove cleanup before and after reopen.

## Public API And File-Format Impact

No public `libmylite` API or storage file-format change is required.

## Storage-Engine Routing Impact

The fixture uses `ENGINE=InnoDB`, which resolves to effective `MYLITE` through
the existing routing policy.

## Binary-Size And Dependency Impact

No dependency or binary-size-sensitive runtime code is added. The change is a
test fixture plus documentation.

## Test And Verification Plan

- Add the SQL fixture.
- Add a storage-engine smoke test that imports and validates the fixture before
  and after close/reopen.
- Update compatibility, storage, roadmap, and related slice docs.
- Run format, targeted storage-smoke tests, CHECK/generated/application-schema
  harness reports, tidy, full preset tests, shell checks, and `git diff --check`.

## Acceptance Criteria

- The dump-style fixture imports into a MyLite-routed schema.
- Imported CHECK and generated-column metadata is enforced before and after
  close/reopen.
- Generated unique and secondary index reads work after import and reopen.
- Docs distinguish representative import coverage from full dump/export
  compatibility.

## Risks And Unresolved Questions

- The fixture runner intentionally supports a narrow SQL subset. Full
  `mysqldump` import will need a separate compatibility slice.
- Exporting MariaDB-compatible SQL from a `.mylite` file remains planned.

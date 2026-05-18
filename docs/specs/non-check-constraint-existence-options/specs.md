# Non-CHECK Constraint Existence Options

## Goal

Broaden non-CHECK constraint DDL coverage for MariaDB existence-option syntax
that maps to MyLite-maintained unique keys:

- existing `DROP CONSTRAINT IF EXISTS unique_name`;
- missing `ADD CONSTRAINT unique_name UNIQUE IF NOT EXISTS (...)`.

## Non-Goals

- Do not claim exhaustive SQL-standard constraint syntax.
- Do not add online or in-place constraint changes.
- Do not cover foreign-key action semantics.
- Do not change primary-key naming behavior; MariaDB still exposes primary
  keys as `PRIMARY`.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:8005-8013` parses `ADD CONSTRAINT` and allows
  `UNIQUE IF NOT EXISTS`.
- `mariadb/sql/sql_yacc.yy:8040-8048` parses
  `DROP CONSTRAINT [IF EXISTS] name`.
- `mariadb/sql/sql_table.cc:11166-11220` resolves non-CHECK
  `DROP CONSTRAINT` to foreign-key or key drops when the name matches those
  objects.
- MyLite copy rebuilds already maintain supported unique-key metadata and row
  checks through close/reopen.

## Compatibility Impact

This narrows the broader non-CHECK constraint matrix gap for routed MyLite
tables. Existing unique constraints can be removed with the existence-option
form, and missing `ADD CONSTRAINT ... UNIQUE IF NOT EXISTS` publishes a new
maintained unique key.

## Affected MariaDB Subsystems

- SQL parser and ALTER-table copy rebuild path.
- MyLite handler/index maintenance for copy-rebuilt unique keys.
- Catalog-backed table definition rediscovery.

## Design

Extend the existing non-CHECK constraint smoke:

1. Reopen a table with initial primary/unique constraints.
2. Drop the initial unique key through `DROP CONSTRAINT IF EXISTS`.
3. Verify the index disappears and duplicate values for the dropped key can be
   inserted.
4. Add a new unique constraint with `ADD CONSTRAINT ... UNIQUE IF NOT EXISTS`.
5. Verify it enforces duplicates and survives another close/reopen.

## DDL Metadata Routing Impact

No new metadata record type is introduced. The table definition image changes
through MariaDB's copy-ALTER path and remains catalog-backed.

## Single-File And Embedded Lifecycle

All durable metadata, rows, and indexes remain in the primary `.mylite` file.
The smoke keeps the sidecar gate in place.

## Public API Or File-Format Impact

None.

## Storage-Engine Routing Impact

The behavior applies to supported MyLite-routed table and unique-key shapes.

## Wire-Protocol Or Integration-Package Impact

None.

## Build, Size, And Dependencies

No dependency or intended size-profile change is introduced.

## Test Plan

- Extend `test_non_check_constraint_ddl()`.
- Run the focused storage-engine smoke binary.
- Run `ctest --preset storage-smoke-dev`, `ctest --preset dev`, and
  `git diff --check`.

## Acceptance Criteria

- Existing `DROP CONSTRAINT IF EXISTS unique_name` removes a maintained unique
  key.
- Duplicate values for the dropped unique key can be inserted.
- Missing `ADD CONSTRAINT unique_name UNIQUE IF NOT EXISTS (...)` creates and
  enforces a maintained unique key.
- The final unique key survives close/reopen.

## Implementation Status

Implemented in storage-engine smoke coverage.

## Risks And Unresolved Questions

- Broader unique syntax variants, generated-column combinations, and conflict
  matrices still need separate coverage.

# Non-CHECK Constraint DDL

## Goal

Cover representative non-CHECK constraint DDL that MariaDB maps to supported
MyLite-maintained keys: named primary-key and unique constraints in `CREATE
TABLE`, `ALTER TABLE ... ADD CONSTRAINT`, and `ALTER TABLE ... DROP
CONSTRAINT`.

## Non-Goals

- Do not implement foreign-key metadata or enforcement.
- Do not add support for unsupported physical index classes such as FULLTEXT,
  SPATIAL, or unbounded long unique BLOB/TEXT keys.
- Do not implement online/in-place constraint changes.
- Do not claim exhaustive SQL-standard constraint coverage.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:6136-6152` parses
  `CONSTRAINT name PRIMARY KEY (...)` and `CONSTRAINT name UNIQUE (...)` through
  the same `Lex->add_key()` path used for ordinary primary and unique key DDL.
- `mariadb/sql/sql_yacc.yy:7236-7238` maps `PRIMARY KEY` to `Key::PRIMARY` and
  `UNIQUE [KEY|INDEX]` to `Key::UNIQUE`.
- `mariadb/sql/sql_yacc.yy:8005-8012` parses `ALTER TABLE ... ADD
  constraint_def`, including `ADD CONSTRAINT name UNIQUE IF NOT EXISTS (...)`.
- `mariadb/sql/sql_yacc.yy:8040-8048` parses `ALTER TABLE ... DROP CONSTRAINT
  [IF EXISTS] name` as a check-constraint drop before later resolution.
- `mariadb/sql/sql_table.cc:11166-11220` rewrites `DROP CONSTRAINT` to
  `DROP FOREIGN KEY` or `DROP KEY` when the named constraint matches a foreign
  key or unique key, so supported unique constraints should use the existing
  copy-rebuild index drop path.
- `mariadb/storage/mylite/ha_mylite.cc:1578-1601` stores the MariaDB table
  definition image in the MyLite catalog for supported table shapes, preserving
  key metadata for close/reopen discovery.
- `mariadb/storage/mylite/ha_mylite.cc` maintains supported primary and unique
  index entries for inserts, updates, deletes, copy rebuilds, and reopen
  discovery through the existing row/index storage paths.

## Compatibility Impact

Covered by this slice:

- Named primary-key and unique constraints in initial routed `CREATE TABLE`
  definitions.
- `ALTER TABLE ... ADD CONSTRAINT name UNIQUE (...)` through copy rebuilds.
- `ALTER TABLE ... ADD CONSTRAINT name UNIQUE IF NOT EXISTS (...)` duplicate
  skips, missing adds, and warning capture where applicable.
- `ALTER TABLE ... DROP CONSTRAINT name` and
  `DROP CONSTRAINT IF EXISTS name` for unique constraints, including
  close/reopen before the drop.
- `ALTER TABLE ... DROP CONSTRAINT IF EXISTS missing_name` skip behavior.
- Duplicate-key enforcement and forced-index reads before and after close/reopen
  for supported constraint-backed unique keys.
- Composite named unique constraints added through copy ALTER, including
  duplicate tuple checks, forced-index reads, close/reopen, and drop behavior.
- Generated-column unique constraints added and dropped through copy ALTER.

Still planned:

- Foreign-key metadata and enforcement.
- Broader non-CHECK constraint syntax matrices.
- Unsupported index classes, online/in-place algorithms, and full SQL
  rollback.

## Proposed Design

No new MyLite storage format or handler API is required. The implementation
should add storage-smoke coverage that proves MariaDB's supported non-CHECK
constraint syntax arrives at MyLite as maintained key metadata:

1. Create a routed `ENGINE=InnoDB` table with `CONSTRAINT ... PRIMARY KEY` and
   `CONSTRAINT ... UNIQUE`.
2. Verify duplicate-key rejection, forced-index reads, and `SHOW INDEX`
   metadata.
3. Add a named unique constraint with `ALTER TABLE ... ADD CONSTRAINT ...`,
   verify duplicate-key rejection, then close/reopen and repeat the checks.
4. Exercise duplicate and missing `ADD CONSTRAINT ... UNIQUE IF NOT EXISTS`
   paths plus existing and missing `DROP CONSTRAINT IF EXISTS` paths.
5. Drop a unique constraint with `DROP CONSTRAINT`, verify the index disappears,
   forced-index reads fail, and duplicate values are accepted.
6. Add another unique constraint after the drop and verify it persists after
   close/reopen.

## Affected Subsystems

- `packages/libmylite/tests/embedded_storage_engine_test.c` storage-smoke
  routed DDL/DML coverage.
- Compatibility, roadmap, and harness documentation.

## DDL Metadata Routing Impact

The covered constraints are key metadata in MariaDB's table definition image.
MyLite must preserve and rediscover them through the same catalog-backed table
definition path used by supported index DDL.

## Single-File And Lifecycle Impact

No new companion file type is introduced. The test must pass sidecar gates and
prove constraint-backed key metadata survives close/reopen in the primary
`.mylite` file.

## Public API And File-Format Impact

No public API or storage file-format change is expected.

## Storage-Engine Routing Impact

The covered constraints are valid only for key shapes MyLite already maintains.
Unsupported key classes and foreign keys remain rejected by existing policy.

## Binary-Size, License, And Dependency Impact

No dependency, license, or MariaDB binary-size impact is expected because the
slice adds tests and docs only unless investigation exposes a bug.

## Test Plan

1. Add a storage-smoke test for named primary/unique constraints in routed
   `CREATE TABLE`.
2. Cover `ALTER TABLE ... ADD CONSTRAINT ... UNIQUE`, duplicate
   `IF NOT EXISTS`, `DROP CONSTRAINT IF EXISTS` missing skips, and
   `DROP CONSTRAINT` over an existing unique key.
3. Verify duplicate-key behavior, forced-index behavior, `SHOW INDEX`, catalog
   metadata counts, close/reopen visibility, and sidecar gates.
4. Run the focused storage-smoke test, routed DDL/DML, unsupported-index and
   sidecar harness reports, formatting, shell checks, clang-tidy, and full
   `dev`, `embedded-dev`, and `storage-smoke-dev` gates.

## Acceptance Criteria

- Supported primary/unique constraint syntax produces maintained MyLite key
  metadata.
- Constraint-backed unique keys reject duplicates and can be used by forced
  index reads before and after close/reopen.
- Dropping a unique constraint through `DROP CONSTRAINT` removes the maintained
  key and allows duplicate values afterward.
- Missing add existence-option paths publish new supported constraints, while
  duplicate add and missing drop existence-option paths warn without mutating
  table state.
- Durable sidecar gates pass.

## Implementation Status

Implemented:

- Storage-smoke coverage creates a routed `ENGINE=InnoDB` table with
  `CONSTRAINT ... PRIMARY KEY` and `CONSTRAINT ... UNIQUE` definitions.
- The test verifies `SHOW INDEX` metadata, duplicate-key rejection, forced-index
  reads, catalog metadata, and sidecar gates for the initial constraints.
- `ALTER TABLE ... ADD CONSTRAINT ... UNIQUE`, duplicate
  `ADD CONSTRAINT ... UNIQUE IF NOT EXISTS`, and missing
  `ADD CONSTRAINT ... UNIQUE IF NOT EXISTS` are covered through copy rebuilds.
- `ALTER TABLE ... DROP CONSTRAINT IF EXISTS missing_name`,
  `DROP CONSTRAINT IF EXISTS slug_unique`, and `DROP CONSTRAINT author_unique`
  are covered after close/reopen.
- The test proves the dropped unique constraint stops enforcing duplicates and
  that a later added unique constraint survives another close/reopen cycle.
- The `primary-key-alter-ddl` follow-up slice covers primary-key add/drop/re-add
  through copy ALTER, including duplicate `ADD PRIMARY KEY IF NOT EXISTS`
  warnings and failed re-add over duplicate rows.
- The `failed-add-unique-constraint-rollback` follow-up slice covers failed
  `ADD CONSTRAINT ... UNIQUE` over duplicate existing rows preserving the old
  table, then successful add after duplicate cleanup.
- The `composite-unique-constraint-ddl` follow-up slice covers named composite
  unique constraints added and dropped through copy ALTER.
- The `generated-unique-constraint-ddl` follow-up slice covers named unique
  constraints over supported generated columns.

No MariaDB source, public API, storage file-format, dependency, license, or
binary-size change was required.

## Risks And Unresolved Questions

- MariaDB treats primary-key names specially; this slice verifies primary-key
  constraint syntax but does not claim custom primary-key name preservation.
- Constraint support remains bounded by the same key-shape limits as existing
  MyLite indexes.

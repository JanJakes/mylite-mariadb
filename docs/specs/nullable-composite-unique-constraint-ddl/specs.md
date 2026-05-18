# Nullable Composite Unique Constraint DDL

## Goal

Cover `ALTER TABLE ... ADD CONSTRAINT ... UNIQUE` for a composite key that
contains nullable columns. Existing coverage proves ordinary nullable unique
indexes and non-null composite unique constraints separately; this slice covers
the combined constraint-backed key shape.

## Non-Goals

- Do not add exhaustive nullable-key permutations.
- Do not change nullable unique-key semantics inherited from MariaDB.
- Do not add online or in-place ALTER support.
- Do not change the MyLite public API, file format, or size profile.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc:mysql_prepare_alter_table()` prepares key metadata
  for copy `ALTER TABLE ... ADD CONSTRAINT ... UNIQUE`.
- `mariadb/sql/key.cc:key_copy()` and key comparison helpers represent NULL
  parts in unique-key tuples according to MariaDB row-buffer metadata.
- `mariadb/storage/mylite/ha_mylite.cc` uses the MariaDB key metadata and row
  buffer to maintain supported unique index entries and preserve nullable
  unique-key semantics.

## Compatibility Impact

This narrows the broader non-CHECK constraint matrix by verifying MariaDB's
nullable unique-key semantics through named composite unique constraints: exact
non-NULL duplicate tuples are rejected, while rows with NULL in nullable key
parts can coexist.

## Design

Add storage-engine smoke coverage that:

1. creates a routed table with nullable composite-key columns,
2. inserts rows whose nullable key parts repeat with NULL values,
3. adds a named composite unique constraint through copy ALTER,
4. verifies non-NULL duplicate tuples fail,
5. verifies repeated NULL-bearing tuples are accepted,
6. verifies forced-index reads before and after reopen, and
7. drops the constraint and verifies duplicate non-NULL tuples are accepted.

## File Lifecycle

All metadata, rows, and index entries live in the primary `.mylite` file. The
test keeps the existing durable-sidecar gate.

## Embedded Lifecycle And API

No public API change is required. The behavior is visible through ordinary
direct SQL execution.

## Storage-Engine Routing

The table uses explicit `ENGINE=InnoDB`, which routes to MyLite while
preserving the requested engine name.

## Build, Size, And Dependencies

No dependency or intended size-profile change is introduced.

## Test Plan

- Add storage-engine smoke coverage for nullable composite unique constraints.
- Update compatibility, roadmap, and adjacent non-CHECK/composite specs.
- Build `mylite_embedded_storage_engine_test`.
- Run the focused storage-engine smoke binary.
- Run `ctest --preset storage-smoke-dev`, `ctest --preset dev`, and
  `git diff --check`.

## Acceptance Criteria

- Copy ALTER adds a named composite unique constraint with nullable key parts.
- Exact non-NULL duplicate tuples are rejected.
- Repeated tuples with NULL in nullable key parts are accepted.
- Forced-index reads work before and after reopen.
- Dropping the constraint removes duplicate enforcement.

## Risks And Unresolved Questions

- This is representative matrix coverage, not an exhaustive nullable unique-key
  suite across all type families or collation modes.

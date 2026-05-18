# Generated Unique Constraint Matrix

## Goal

Expand generated-column unique constraint coverage beyond a single stored
generated column. The covered matrix adds a composite unique constraint over a
base column plus a virtual generated column, then verifies existence-option DDL
and drop behavior.

## Non-Goals

- Do not add MySQL-style expression index syntax.
- Do not cover BLOB/TEXT generated columns or generated primary keys.
- Do not add exhaustive generated expression coverage.
- Do not change the MyLite public API, file format, or size profile.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc:mysql_prepare_alter_table()` prepares key lists
  for copy `ALTER TABLE ... ADD CONSTRAINT ... UNIQUE`.
- `mariadb/sql/table.cc:TABLE::update_virtual_fields()` computes virtual
  generated values before write-time key construction.
- `mariadb/sql/key.cc:key_copy()` builds key tuples from the MariaDB row buffer,
  including generated values already computed by the SQL layer.
- `mariadb/storage/mylite/ha_mylite.cc` validates MyLite-supported key shapes,
  stores retained key metadata, and maintains append-only index entries for
  supported copy-ALTER indexes.

## Compatibility Impact

This narrows the broader non-CHECK constraint matrix and generated-column
matrix gaps. It covers the MariaDB-supported shape applications can use for
tenant- or site-scoped slugs: `UNIQUE (site_id, generated_slug)`.

Expression indexes, generated primary keys, and BLOB/TEXT generated unique
constraints remain governed by their existing unsupported or planned status.

## Design

Add storage-engine smoke coverage that:

1. creates a routed `ENGINE=InnoDB` table with a virtual generated slug,
2. inserts rows with duplicate generated slugs under different site ids,
3. adds a named composite unique constraint over `(site_id, slug)`,
4. verifies duplicate insert and update failures for the same site/slug pair,
5. verifies forced-index reads through the generated composite key,
6. reopens and checks retained index metadata,
7. verifies `ADD CONSTRAINT ... UNIQUE IF NOT EXISTS` and
   `DROP CONSTRAINT IF EXISTS` paths, and
8. verifies dropping the constraint removes duplicate enforcement.

## File Lifecycle

All table metadata, generated-column metadata, and composite generated-index
entries remain in the primary `.mylite` file. The test keeps the existing
durable-sidecar gate.

## Embedded Lifecycle And API

No public API change is required. The behavior is visible through ordinary
direct SQL execution.

## Storage-Engine Routing

The table uses explicit `ENGINE=InnoDB`, which routes to MyLite while
preserving the requested engine name.

## Build, Size, And Dependencies

No dependency or intended size-profile change is introduced.

## Test Plan

- Add storage-engine smoke coverage for a composite virtual generated unique
  constraint and existence-option DDL.
- Update compatibility, roadmap, and adjacent generated/non-CHECK specs.
- Build `mylite_embedded_storage_engine_test`.
- Run the focused storage-engine smoke binary.
- Run `ctest --preset storage-smoke-dev`, `ctest --preset dev`, and
  `git diff --check`.

## Acceptance Criteria

- Duplicate generated slugs are allowed across different site ids.
- The composite generated unique constraint rejects duplicate insert and update
  paths for the same site id.
- Forced-index reads work before and after reopen.
- `IF NOT EXISTS` and `IF EXISTS` constraint forms preserve catalog and index
  state.
- Dropping the generated unique constraint removes duplicate enforcement.

## Risks And Unresolved Questions

- This is representative matrix coverage, not an exhaustive generated-column
  constraint suite.

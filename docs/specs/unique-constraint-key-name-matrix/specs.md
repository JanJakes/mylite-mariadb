# Unique Constraint Key-Name Matrix

## Goal

Cover the MariaDB non-CHECK constraint syntax where
`ADD CONSTRAINT logical_name UNIQUE KEY physical_name (...)` stores and drops
the supported MyLite key by the physical key name, not by the optional
constraint label.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:6104-6148` parses primary and unique key
  definitions through `key_def`.
- `mariadb/sql/sql_yacc.yy:6132-6140` chooses the explicit key identifier for
  `CONSTRAINT name UNIQUE KEY explicit_name (...)`; only when the explicit key
  identifier is absent does it use the constraint label as the key name.
- `mariadb/sql/sql_yacc.yy:7978-8008` routes `ALTER TABLE ... ADD key_def`
  through the normal index-add alter path.
- `mariadb/sql/sql_yacc.yy:8040-8048` parses `DROP CONSTRAINT [IF EXISTS]` as
  a check-constraint drop before later resolution.
- `mariadb/sql/sql_table.cc:11282-11321` resolves `DROP CONSTRAINT` to
  `DROP KEY` only when the requested name matches an existing unique key name.

## Scope

- Durable MyLite-routed `ENGINE=InnoDB` tables.
- Supported base-column unique constraints added through copy `ALTER`.
- Explicit logical constraint label plus explicit physical unique-key name.
- Missing-label `DROP CONSTRAINT IF EXISTS` warning behavior.
- Physical-key `DROP CONSTRAINT` removal, duplicate acceptance after drop, and
  close/reopen persistence.

## Non-Goals

- Foreign-key constraint-name matrices.
- Primary-key custom-name preservation; MariaDB treats primary-key names
  specially.
- Unsupported physical key classes, online/in-place algorithms, expression
  indexes, partitions, or full SQL transaction/savepoint rollback.

## Compatibility Impact

This narrows the broader non-CHECK constraint matrix gap by proving that MyLite
inherits MariaDB's physical-key-name semantics for unique constraints:

- the explicit unique-key name is the maintained and discoverable key name;
- the optional constraint label is not exposed as a unique key name when an
  explicit key name is present;
- `DROP CONSTRAINT IF EXISTS logical_label` warns and leaves the key intact;
  and
- `DROP CONSTRAINT physical_key_name` removes the maintained key.

## Design

No production change is expected. MariaDB's parser and ALTER resolution already
turn the syntax into supported key metadata, and MyLite's copy-rebuild path
maintains the resulting index entries.

## File Lifecycle

No file-format or companion-file change is required. The table definition,
rows, and index entries remain in the primary `.mylite` file, with no durable
MariaDB sidecars.

## Embedded Lifecycle And API

No public `libmylite` API change is required. The behavior is observable
through SQL execution, `SHOW INDEX`, duplicate-key enforcement, warning
enumeration, and close/reopen discovery.

## Storage-Engine Routing

The test uses explicit `ENGINE=InnoDB`, which routes to MyLite while preserving
the requested engine name.

## Build, Size, And Dependencies

No dependency, license, or intended size-profile change is introduced.

## Test Plan

- Add storage-engine smoke coverage for
  `ALTER TABLE ... ADD CONSTRAINT logical_name UNIQUE KEY physical_name (...)`.
- Verify `SHOW INDEX` exposes only the physical key name.
- Verify duplicate-key enforcement and forced-index reads through the physical
  key name.
- Verify `DROP CONSTRAINT IF EXISTS logical_name` warns and preserves the key.
- Verify `DROP CONSTRAINT physical_name` removes the key and allows duplicates.
- Verify close/reopen keeps the dropped-key state and durable sidecar gates
  pass.
- Run the focused storage-engine test, routed DDL/DML compatibility harness
  group, shell syntax checks, `git diff --check`, and the dev, embedded-dev,
  and storage-smoke presets.

## Acceptance Criteria

- Explicit physical key names on unique constraints are maintained by MyLite.
- The logical constraint label does not appear as a duplicate unique-key name.
- Dropping the logical label with `IF EXISTS` warns without mutating the key.
- Dropping the physical key name removes the unique key before and after
  close/reopen.
- Docs distinguish this representative matrix from broader non-CHECK
  constraint work.

## Risks And Open Questions

- The slice covers the unique-key path only. Foreign-key names have separate
  metadata and action semantics.
- The slice does not change MyLite's bounded supported-key-shape policy.

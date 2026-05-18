# Composite Unique Constraint DDL

## Goal

Broaden non-CHECK constraint coverage to named composite unique constraints
added and dropped through copy `ALTER TABLE`.

## Non-Goals

- Do not add support for unsupported physical index classes.
- Do not implement online, in-place, instant, or no-copy constraint changes.
- Do not cover foreign keys or primary-key naming behavior.
- Do not claim exhaustive multi-column constraint matrices.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:6136-6145` parses named primary/unique constraint
  definitions through `Lex->add_key()` and a `key_list`, so composite column
  lists use the same parser path as ordinary composite keys.
- `mariadb/sql/sql_yacc.yy:7236-7238` maps `PRIMARY KEY` and
  `UNIQUE [KEY|INDEX]` to key types.
- `mariadb/sql/sql_yacc.yy:8005-8012` parses
  `ALTER TABLE ... ADD constraint_def`.
- `mariadb/sql/sql_table.cc:11166-11220` resolves non-CHECK
  `DROP CONSTRAINT` to a key drop when the name matches a unique key.

## Compatibility Impact

This narrows the broader non-CHECK constraint matrix gap. Supported composite
unique constraints added through `ALTER TABLE ... ADD CONSTRAINT` should publish
ordinary maintained MyLite composite unique-key metadata, enforce duplicate
tuples, support forced-index reads, survive close/reopen, and stop enforcing
after `DROP CONSTRAINT`.

## Affected MariaDB Subsystems

- SQL parser and copy ALTER path.
- MyLite catalog-backed table-definition rediscovery.
- MyLite composite unique index-entry maintenance.

## Design

Add a storage-smoke test that:

1. Creates a routed table without a composite unique key.
2. Adds `CONSTRAINT site_slug_unique UNIQUE (site_id, slug)` through copy
   `ALTER TABLE`.
3. Verifies `SHOW INDEX` exposes both key parts.
4. Verifies duplicate tuple rejection and same-slug/different-site acceptance.
5. Verifies forced-index reads before and after close/reopen.
6. Drops the constraint and verifies duplicate tuples can be inserted.

## DDL Metadata Routing Impact

No new metadata record type is introduced. Composite unique constraints remain
key metadata in MariaDB's table-definition image and MyLite's existing index
entry records.

## Single-File And Embedded Lifecycle

All durable table metadata, rows, and index entries remain in the primary
`.mylite` file. The smoke must keep sidecar gates across close/reopen.

## Public API Or File-Format Impact

None.

## Storage-Engine Routing Impact

The behavior applies only to supported MyLite-routed table and key shapes.

## Wire-Protocol Or Integration-Package Impact

None.

## Build, Size, And Dependencies

No dependency or intended size-profile change is introduced.

## Test Plan

- Add `test_composite_unique_constraint_ddl()`.
- Run the focused storage-engine smoke binary.
- Run `ctest --preset storage-smoke-dev`, `ctest --preset dev`, and
  `git diff --check`.

## Acceptance Criteria

- Copy ALTER adds a named composite unique constraint.
- The composite unique key rejects duplicate tuples while allowing non-conflict
  tuples.
- Forced-index reads use the composite constraint-backed key before and after
  close/reopen.
- Dropping the constraint removes the key and allows duplicate tuples.
- Sidecar checks pass.

## Implementation Status

Implemented in storage-engine smoke coverage.

## Risks And Unresolved Questions

- Broader nullable composite unique-key behavior remains covered only by
  existing ordinary unique-key semantics, not a full constraint matrix.

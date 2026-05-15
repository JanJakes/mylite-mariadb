# Autoincrement Key Policy

## Problem

MyLite persists table-local autoincrement state for supported single-column
autoincrement keys. Since general primary and secondary indexes are now
supported, the generic key-shape gate can admit compound keys that include an
`AUTO_INCREMENT` column. That is unsafe: compound autoincrement semantics are
engine-specific, and MyLite has not designed per-prefix allocation or
compatibility rules for those shapes.

This slice makes the current policy explicit: routed tables with an
`AUTO_INCREMENT` column are supported only when that column has a single-part
key over itself. Compound autoincrement-only definitions reject before MyLite
catalog publication.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.cc:handler::update_auto_increment()` delegates value
  allocation to the storage engine through `get_auto_increment()` and uses the
  table's `next_number_field`.
- `mariadb/sql/handler.cc:handler::get_auto_increment()` defaults to reading
  index state to find the next value. MyLite overrides this because its
  autoincrement state is append-only and table-local.
- `mariadb/storage/mylite/ha_mylite.cc:mylite_auto_increment_field()` locates
  the table's MariaDB autoincrement field.
- `mariadb/storage/mylite/ha_mylite.cc:mylite_table_supports_row_write()`
  currently accepts any supported key shape, without checking whether an
  autoincrement field has only a single-column autoincrement key.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::create()` calls the row-write
  support gate before storing a routed table definition in the MyLite catalog.

## Design

Add a handler helper that validates autoincrement key support:

- If a table has no autoincrement field, leave existing key support unchanged.
- If a table has an autoincrement field, require at least one user-defined
  single-part key whose only key part is that field.
- Reject routed autoincrement table definitions that only expose the field
  through compound keys.

The policy deliberately allows additional supported keys when the
autoincrement column also has a complete single-column key. This keeps current
WordPress-shaped and ordinary `PRIMARY KEY(id), KEY(category, id)` schemas
working while blocking the unsupported grouped-allocation shapes.

## Supported Scope

- Single-column autoincrement keys remain supported.
- Additional supported secondary or compound keys remain allowed when the
  autoincrement column also has a single-column key.
- Compound-only autoincrement definitions reject before MyLite catalog
  publication.

## Non-Goals

- Per-prefix autoincrement allocation for MyISAM-style grouped sequences.
- InnoDB-specific compound autoincrement compatibility claims.
- Transaction-aware autoincrement rollback.
- Cross-process autoincrement allocation guarantees beyond existing file locks.

## Compatibility Impact

Autoincrement support remains partial. The compatibility matrix should state
that compound-only autoincrement key definitions are rejected explicitly until
the sequence semantics are designed and tested.

## DDL Metadata Routing Impact

The gate runs before catalog publication through the existing `create()` path,
so rejected compound-only autoincrement tables leave no MyLite table-definition
record behind.

## Single-File And Embedded-Lifecycle Impact

No file-format or companion-file change is required. The slice prevents
unsupported metadata from entering the primary `.mylite` file.

## Public API And File-Format Impact

No public `libmylite` API or storage file-format change is required.

## Storage-Engine Routing Impact

The policy applies to all routed engine names because omitted engine,
`ENGINE=MYLITE`, `ENGINE=InnoDB`, `ENGINE=MyISAM`, and `ENGINE=Aria` share the
same MyLite handler support gate.

## Binary-Size And Dependency Impact

No dependency or binary-size-sensitive runtime code is added.

## Test And Verification Plan

- Add storage-engine smoke coverage that:
  - accepts a single-column autoincrement key with an additional compound
    secondary key,
  - rejects compound-only autoincrement definitions for representative routed
    engines, and
  - proves rejected definitions do not publish catalog metadata.
- Update compatibility, storage architecture, roadmap, and autoincrement specs.
- Run format, targeted storage-smoke tests, routed DDL/DML harness report,
  tidy, full preset tests, shell checks, and `git diff --check`.

## Acceptance Criteria

- Supported single-column autoincrement tables still create, insert, and expose
  generated ids.
- Compound-only autoincrement table definitions fail before MyLite catalog
  publication.
- Compatibility docs distinguish explicit rejection from future compound
  autoincrement support.

## Risks And Unresolved Questions

- Some compound shapes may be acceptable under specific MariaDB engines. MyLite
  should not claim those until it has an engine-specific compatibility design
  and tests.
- Future support may need per-prefix sequence state, index-assisted maximum
  lookup, or both.

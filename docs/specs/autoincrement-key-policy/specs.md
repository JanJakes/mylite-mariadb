# Autoincrement Key Policy

## Problem

MyLite persists table-local autoincrement state for supported autoincrement
keys. Since general primary and secondary indexes are now supported, the
generic key-shape gate can admit compound keys that include an
`AUTO_INCREMENT` column. Some compound shapes are safe for MyLite's
table-local allocation model, while MyISAM/Aria-style grouped sequences require
per-prefix allocation that MyLite had not designed at the time of this slice.

This slice made the initial policy explicit: routed tables with an
`AUTO_INCREMENT` column were supported only when that column had a single-part
key over itself. The follow-up `autoincrement-first-key-compound` slice extends
that policy to support InnoDB-compatible first-key compound autoincrement
definitions while still rejecting grouped-sequence shapes before MyLite catalog
publication.

Current status: the later `autoincrement-grouped-prefix` slice supersedes the
grouped rejection by adding scan-backed per-prefix allocation for routed
tables.

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
  accepts supported key shapes only after the autoincrement field has passed
  the MyLite autoincrement key policy.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::create()` calls the row-write
  support gate before storing a routed table definition in the MyLite catalog.

## Design

Add a handler helper that validates autoincrement key support:

- If a table has no autoincrement field, leave existing key support unchanged.
- If a table has an autoincrement field, require at least one user-defined key
  whose first key part is that field.
- Reject routed autoincrement table definitions that only expose the field
  after another key part.

The policy deliberately allows additional supported keys when the
autoincrement column also has a supported first-key position. This keeps
current WordPress-shaped and ordinary `PRIMARY KEY(id), KEY(category, id)`
schemas working while also allowing `PRIMARY KEY(id, category)`. It still
blocks unsupported grouped-allocation shapes such as
`PRIMARY KEY(category, id)`.

## Supported Scope

- Single-column autoincrement keys remain supported.
- First-key compound autoincrement keys use the same table-local allocation
  state as single-column keys.
- Additional supported secondary or compound keys remain allowed when the
  autoincrement column also has a supported first-key position.
- Grouped-sequence definitions where the autoincrement column appears later in
  a key reject before MyLite catalog publication.

## Non-Goals

- Per-prefix autoincrement allocation for MyISAM-style grouped sequences.
- Transaction-aware autoincrement rollback.
- Cross-process autoincrement allocation guarantees beyond existing file locks.

## Compatibility Impact

Autoincrement support remains partial. First-key compound autoincrement
definitions use table-local allocation. The grouped-prefix slice now covers the
initial scan-backed per-prefix allocation path for later-in-key definitions.

## DDL Metadata Routing Impact

The gate runs before catalog publication through the existing `create()` path.
For this historical slice, rejected grouped-sequence autoincrement tables left
no MyLite table-definition record behind. The grouped-prefix slice later
relaxed that gate for supported routed tables.

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
  - accepts first-key compound autoincrement definitions,
  - originally rejected grouped-sequence autoincrement definitions for
    representative routed engines, and
  - proved rejected definitions did not publish catalog metadata.
- Update compatibility, storage architecture, roadmap, and autoincrement specs.
- Run format, targeted storage-smoke tests, routed DDL/DML harness report,
  tidy, full preset tests, shell checks, and `git diff --check`.

## Acceptance Criteria

- Supported single-column autoincrement tables still create, insert, and expose
  generated ids.
- First-key compound autoincrement table definitions create, insert, and
  preserve table-local generated ids.
- Grouped-sequence autoincrement table definitions failed before MyLite catalog
  publication in this slice; the grouped-prefix slice supersedes this
  criterion.
- Compatibility docs distinguish first-key compound support from future
  grouped-sequence support.

## Risks And Unresolved Questions

- The grouped-prefix slice adds scan-backed grouped-sequence support. Future
  performance work may still need per-prefix sequence state, index-assisted
  maximum lookup, or both.

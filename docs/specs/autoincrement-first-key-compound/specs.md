# Autoincrement First-Key Compound Support

## Goal

Support the InnoDB-compatible compound autoincrement shape where the
`AUTO_INCREMENT` column is the first column of a supported key, even when no
separate single-column autoincrement key exists. Keep grouped sequence shapes,
where the autoincrement column appears later in a compound key, explicitly
unsupported.

## Non-Goals

- MyISAM/Aria-style per-prefix grouped autoincrement allocation.
- Allowing an `AUTO_INCREMENT` column that is not the first part of any
  supported key.
- Transaction-aware autoincrement rollback.
- Cross-process autoincrement allocation guarantees beyond existing file
  locks.
- Changing sequence objects or SQL sequence functions.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- MariaDB's `AUTO_INCREMENT` documentation states that the column must be
  indexed, and for default InnoDB compound keys the autoincrement column must
  be the first column. It also states that Aria and MyISAM permit the column
  elsewhere in a key:
  <https://mariadb.com/docs/server/reference/data-types/auto_increment>.
- `mariadb/sql/handler.cc:handler::update_auto_increment()` still delegates
  allocation through the engine's `get_auto_increment()` hook for this shape.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::get_auto_increment()` already
  reads MyLite's table-local durable next value and applies MariaDB's session
  offset/increment rounding.
- Before this slice,
  `mariadb/storage/mylite/ha_mylite.cc:mylite_table_supports_auto_increment()`
  requires a single-part key over the autoincrement field. That rejects
  InnoDB-compatible compound keys such as `PRIMARY KEY (id, category)`.
- MyLite duplicate checks already operate over supported unique key images.
  For `PRIMARY KEY (id, category)`, uniqueness remains compound-key uniqueness,
  while generated values come from the table-local autoincrement counter.

## Compatibility Impact

Autoincrement support remains partial but now covers the InnoDB-compatible
first-key compound shape. MyLite still does not claim MyISAM/Aria grouped
sequence behavior for `PRIMARY KEY (category, id)` style definitions.

## Design

Relax the handler autoincrement key gate:

- If a table has no autoincrement field, keep existing behavior.
- If a table has an autoincrement field, accept the table when at least one
  supported key has that field as its first key part.
- Continue relying on the existing supported-index gate for key classes,
  lengths, algorithms, and BLOB/TEXT prefix limits.
- Continue rejecting routed tables where the autoincrement field appears only
  after another key part.

No storage format change is needed. MyLite's autoincrement state remains
table-local, so generated values are global per table rather than per
non-autoincrement prefix.

## DDL Metadata Routing Impact

Accepted first-key compound autoincrement tables publish normal MyLite catalog
metadata. Rejected grouped-sequence definitions still fail before table
definition publication.

## Single-File And Embedded-Lifecycle Impact

No new durable companion files are introduced. Rows, index entries, and
autoincrement state continue to live in the primary `.mylite` file.

## Public API And File-Format Impact

No public `libmylite` API or file-format change.

## Storage-Engine Routing Impact

The newly supported shape is safe for routed `ENGINE=InnoDB` semantics because
the autoincrement column is first in the compound key. Requested `ENGINE=MyISAM`
and `ENGINE=Aria` still use the same MyLite behavior for this shape, but
MyISAM/Aria-specific grouped sequences remain rejected when the autoincrement
column is not first.

## Build, Size, And Dependencies

No dependency or meaningful binary-size impact. The code change is a small
handler gate adjustment plus tests.

## Test Plan

- Extend storage-engine smoke coverage so:
  - `PRIMARY KEY (id, category)` with `id AUTO_INCREMENT` succeeds;
  - generated inserts use table-local values across categories;
  - explicit duplicate compound keys still fail;
  - explicit high values advance later generated values;
  - close/reopen preserves the compound shape and next value;
  - `PRIMARY KEY (category, id)` still rejects before catalog publication.
- Update autoincrement policy, compatibility, roadmap, and storage
  architecture docs.
- Run storage-smoke embedded tests, first-party storage tests, format check,
  and `git diff --check`.

## Acceptance Criteria

- InnoDB-compatible first-key compound autoincrement DDL succeeds and routes to
  MyLite.
- Generated values remain table-local and survive close/reopen.
- Grouped-sequence definitions remain explicit failures with no catalog record.
- Docs distinguish first-key compound support from grouped sequence support.

## Risks And Open Questions

- This does not implement per-prefix allocation for MyISAM/Aria grouped
  sequences. Future support would need prefix-specific durable counters or
  index-assisted maximum lookup by group.
- Explicit duplicate behavior follows the declared unique keys. A compound
  primary key can allow repeated autoincrement values when another key part
  differs, while generated values remain table-local.

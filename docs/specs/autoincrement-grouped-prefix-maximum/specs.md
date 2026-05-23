# Autoincrement Grouped Prefix Maximum Lookup

## Goal

Tighten grouped later-in-key `AUTO_INCREMENT` allocation so MyLite uses
MariaDB key comparison over live index entries to find the maximum generated
value for the current non-autoincrement prefix, then reads only the selected
row payload.

## Non-Goals

- Do not add durable B-tree pages or a new storage file format.
- Do not add a public storage API.
- Do not change first-key table-local autoincrement behavior.
- Do not change first-key table-local rollback gap behavior.
- Do not claim exhaustive offset/increment coverage.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/myisam/ha_myisam.cc:ha_myisam::get_auto_increment()`
  copies the key prefix before the autoincrement column and asks MyISAM for
  the last row in that prefix.
- `mariadb/storage/maria/ha_maria.cc:ha_maria::get_auto_increment()` follows
  the same prefix lookup model for Aria.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::get_auto_increment()`
  dispatches grouped later-in-key tables to
  `mylite_read_grouped_auto_increment()`.
- `mariadb/storage/mylite/ha_mylite.cc:mylite_compare_key_tuple()` compares
  MyLite key tuples through MariaDB key-part comparison, including field
  collation handling for the stored key images used by MyLite.
- `packages/mylite-storage/src/storage.c:mylite_storage_read_index_entries()`
  and `mariadb/storage/mylite/mylite_volatile_rows.cc`
  `mylite_volatile_read_index_entries()` expose only live entries, filtering
  deleted or replaced rows through row-state data.

## Compatibility Impact

Generated values remain MyISAM/Aria-style per-prefix values for routed tables,
including explicit `ENGINE=InnoDB` declarations that resolve to MyLite. The
selected row is still derived from live rows in the current prefix, so stale
deleted or updated high index entries do not advance the prefix.

## Affected MariaDB Subsystems

- MyLite handler grouped `AUTO_INCREMENT` allocation.
- MyLite live index-entry readers for durable and volatile rows.
- MariaDB SQL-layer `handler::update_auto_increment()` flow through ordinary
  routed inserts.

## Design

Keep the existing handler contract:

1. first-key autoincrement tables use durable table-local state;
2. grouped later-in-key tables serialize the current row's prefix before the
   autoincrement part;
3. the handler reads live entries for the grouped key;
4. matching entries are compared with the same key tuple comparison used by
   MyLite index cursors;
5. the handler keeps the greatest matching key tuple for the current MariaDB
   stored key image;
6. the selected row is fetched and decoded to return `max(prefix value) + 1`,
   rounded later by the existing offset/increment path.

This is a handler-level maximum lookup over the current live entryset. It does
not introduce durable B-tree pages or an O(log n) storage primitive.

## File Lifecycle

No file-format or companion-file change is introduced. The primary `.mylite`
file continues to contain row-state pages and append-only index-entry pages.

## DDL Metadata Routing Impact

No DDL metadata change is introduced. Existing grouped autoincrement
definitions remain catalog-backed and reopen through MyLite metadata discovery.

## Embedded Lifecycle And API

No public `libmylite` API change is required. The behavior is visible through
ordinary SQL inserts into MyLite-routed tables.

## Storage-Engine Routing

The behavior applies to durable MyLite-routed tables and runtime-volatile
`MEMORY` / `HEAP` tables that use the MyLite handler. Requested `InnoDB`,
`MyISAM`, and `Aria` engines continue to route to MyLite for supported table
shapes.

## Build, Size, And Dependencies

No dependency or intended size-profile change is introduced.

## Test Plan

- Extend grouped autoincrement smoke coverage with an updated explicit high
  value to prove stale replaced index entries do not drive the next generated
  value.
- Cover a reverse-sort grouped autoincrement definition so allocation remains
  numeric even when broader reverse-sort index navigation is separate work.
- Re-run the focused storage-engine smoke binary.
- Run `ctest --preset storage-smoke-dev`, `ctest --preset dev`, and
  `git diff --check`.

## Acceptance Criteria

- Grouped generated values continue to allocate per exact key prefix.
- Deleted or updated stale high values do not affect the next generated value.
- Reverse-sort grouped autoincrement definitions still choose the highest
  numeric value for allocation.
- First-key table-local autoincrement behavior remains unchanged.
- Docs distinguish this live-entry maximum lookup from future durable B-tree
  page work.

## Implementation Status

Implemented. `mylite_read_grouped_auto_increment()` now selects the maximum
matching live index entry by key comparison and fetches only that row payload
before applying the existing offset/increment rounding path.
Durable entryset materialization was later narrowed by
[Index Prefix Entryset Read](../index-prefix-entryset-read/specs.md), which
lets storage return only entries matching the serialized prefix.
Runtime-volatile entryset materialization was later narrowed by
[Volatile Index Prefix Entryset Read](../volatile-index-prefix-entryset-read/specs.md)
for the same grouped allocation path.

## Risks And Unresolved Questions

- Durable append-tail fallback and volatile grouped allocation still scan their
  narrowed prefix entry streams. Durable B-tree pages or a storage-level
  prefix-maximum primitive remain future work.
- Grouped transaction and savepoint rollback coverage is tracked by
  [Autoincrement Grouped Transaction Rollback](../autoincrement-grouped-transaction-rollback/specs.md).

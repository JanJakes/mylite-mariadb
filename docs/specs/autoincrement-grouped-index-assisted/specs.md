# Autoincrement Grouped Index-Assisted Allocation

## Goal

Replace MyLite's grouped later-in-key `AUTO_INCREMENT` table-row scan with an
index-assisted path that uses live MyLite index entries for the grouped key,
then reads only rows in the current key prefix to compute the next generated
value.

## Non-Goals

- Do not add a new storage file format or public storage API.
- Do not implement B-tree pages or an O(log n) prefix-maximum lookup.
- Do not change first-key table-local autoincrement behavior.
- Do not add transaction-aware rollback of consumed generated values.
- Do not claim exhaustive offset/increment coverage.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/myisam/ha_myisam.cc:ha_myisam::get_auto_increment()` copies
  the key prefix before the autoincrement column and asks MyISAM for the last
  row in that prefix.
- `mariadb/storage/maria/ha_maria.cc:ha_maria::get_auto_increment()` follows
  the same prefix lookup model for Aria.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::get_auto_increment()`
  dispatches grouped later-in-key tables to
  `mylite_read_grouped_auto_increment()`.
- `packages/mylite-storage/src/storage.c:mylite_storage_read_index_entries()`
  returns only live index entries by building the row-state map and validating
  each referenced row before appending the entry.
- `mariadb/storage/mylite/mylite_volatile_rows.cc`
  `mylite_volatile_read_index_entries()` applies the same live-row filtering
  for runtime-volatile `MEMORY` / `HEAP` rows.

## Compatibility Impact

Generated values remain MyISAM/Aria-style per-prefix values for routed tables,
including explicit `ENGINE=InnoDB` tables that resolve to MyLite. The behavior
still derives the next value from current live rows in the prefix, so deleted
high values do not keep advancing a prefix.

## Affected MariaDB Subsystems

- MyLite handler `get_auto_increment()` implementation.
- MyLite durable and volatile row/index read paths.
- SQL-layer autoincrement allocation through MariaDB's existing
  `handler::update_auto_increment()` flow.

## Design

Keep the existing `get_auto_increment()` contract:

1. first-key autoincrement tables continue to use durable table-local state;
2. grouped later-in-key tables serialize the current row's key prefix before
   the autoincrement part;
3. the handler reads live index entries for the grouped key;
4. entries whose key bytes do not start with the current prefix are skipped;
5. matching entries fetch their live row payload by row id and decode the
   autoincrement field from the MariaDB record image;
6. the handler returns `max(prefix auto_increment value) + 1`, rounded by the
   session offset/increment path already used by MariaDB.

This removes full row materialization from grouped allocation. It still scans
the current append-only index-entry stream, because MyLite does not yet have a
B-tree or prefix-maximum primitive.

## File Lifecycle

No file-format or companion-file change is introduced. The primary `.mylite`
file already contains row-state pages and append-only index-entry pages.
Stale deleted or replaced rows remain filtered by the storage index reader.

## DDL Metadata Routing Impact

No DDL metadata change is introduced. Existing routed grouped autoincrement
table definitions remain catalog-backed and continue to reopen through MyLite
metadata discovery.

## Embedded Lifecycle And API

No public `libmylite` API change is required. The behavior is visible through
ordinary SQL inserts into MyLite-routed tables.

## Public API Or File-Format Impact

No public API, storage header, page format, or catalog format change is
introduced.

## Storage-Engine Routing

The behavior applies to MyLite-routed durable tables and runtime-volatile
`MEMORY` / `HEAP` tables that use the MyLite handler. Explicit `ENGINE=InnoDB`,
`ENGINE=MyISAM`, and `ENGINE=Aria` declarations continue to route to MyLite
for the supported table shape.

## Build, Size, And Dependencies

No dependency or intended size-profile change is introduced. The change is
handler logic plus smoke coverage.

## Wire-Protocol Or Integration-Package Impact

None. This is an embedded handler behavior change only.

## License Or Dependency Impact

None.

## Test Plan

- Extend grouped autoincrement smoke coverage with a deleted explicit high
  value to prove stale index entries do not drive the next generated value.
- Re-run the focused storage-engine smoke binary.
- Run `ctest --preset storage-smoke-dev`, `ctest --preset dev`, and
  `git diff --check`.

## Acceptance Criteria

- Grouped generated values continue to allocate per exact key prefix.
- Deleted high values in a prefix no longer affect the next generated value.
- First-key table-local autoincrement behavior remains unchanged.
- Docs distinguish the implemented live-index-entry path from future B-tree or
  prefix-maximum lookup work.

## Implementation Status

Implemented, then tightened by
[Autoincrement Grouped Prefix Maximum Lookup](../autoincrement-grouped-prefix-maximum/specs.md).
The first implementation read live index entries for the grouped key, filtered
them by the current serialized prefix, and fetched matching rows to compute the
prefix maximum. The follow-up maximum-lookup slice now chooses the maximum by
comparing matching live entries and fetches only the selected maximum row.

## Risks And Unresolved Questions

- The follow-up maximum lookup still scans the append-only index-entry stream.
  It is not a durable B-tree page implementation.
- Grouped autoincrement still lacks transaction-aware rollback of consumed
  generated values.

# BLACKHOLE Engine Routing

## Problem

MyLite routes durable application table engines such as `InnoDB`, `MyISAM`, and
`Aria` to the MyLite handler, but it still rejects zero-file engine requests.
`BLACKHOLE` is the smallest safe zero-file engine to support first: MariaDB
keeps table metadata but discards all rows written to the table. That maps to
MyLite's single-file model without adding native BLACKHOLE plugin files or
pretending that discarded rows are durable MyLite data.

`MEMORY` / `HEAP` should not be folded into this slice. Those tables preserve
metadata while row contents are process-lifetime state. Routing them to durable
MyLite row pages would be easier, but it would be semantically wrong.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/blackhole/ha_blackhole.cc:87-92` makes native BLACKHOLE
  table creation a metadata-only handler operation.
- `mariadb/storage/blackhole/ha_blackhole.cc:104-108` accepts `write_row()`
  and only updates autoincrement state when needed.
- `mariadb/storage/blackhole/ha_blackhole.cc:128-152` returns no rows from
  full scans or positioned reads during ordinary non-replication execution.
- `mariadb/storage/blackhole/ha_blackhole.cc:228-305` returns no rows from
  index reads during ordinary non-replication execution.
- `mariadb/storage/blackhole/ha_blackhole.h:48-67` advertises broad index and
  blob capabilities for the native engine, but MyLite should keep its current
  supported-index policy until those capabilities are implemented in MyLite.
- `mariadb/sql/handler.cc:133-139` aliases `HEAP` and `MEMORY`, making MEMORY
  a separate routing design from BLACKHOLE.
- MariaDB's BLACKHOLE documentation describes it as a table definition with no
  associated data or index files:
  <https://mariadb.com/docs/server/server-usage/storage-engines/blackhole>.

## Scope

- Accept explicit `ENGINE=BLACKHOLE` table creation through the MyLite handler.
- Store requested engine metadata as `BLACKHOLE` with effective engine `MYLITE`.
- Keep table definitions discoverable after close/reopen.
- Discard inserted rows and return empty results from full scans and supported
  index reads.
- Preserve MyLite's existing unsupported-index checks for routed BLACKHOLE
  table definitions.
- Keep durable sidecar checks clean.

## Non-Goals

- Native BLACKHOLE plugin integration.
- BLACKHOLE replication/binlog behavior.
- Full native BLACKHOLE index capability parity.
- `MEMORY` / `HEAP` routing or volatile row storage.
- Transaction, savepoint, crash-recovery, or concurrency changes.

## Design

Extend the MyLite handler's supported requested-engine set with `BLACKHOLE`.
The catalog continues to store the canonical MariaDB table definition and the
requested/effective engine names in the primary `.mylite` file.

When a handler instance opens or creates a table whose requested engine is
`BLACKHOLE`, mark it as row-discarding:

1. `write_row()` succeeds without appending row or index-entry pages.
2. Full-scan and index cursors initialize as empty.
3. Positioned reads return end-of-file.
4. `truncate()` succeeds and clears any local cursor state.
5. `info()` reports no durable row data for ordinary query planning.

DDL still passes through MyLite's current table-shape gates. That means
unsupported FULLTEXT, SPATIAL, unbounded long unique BLOB/TEXT, expression-key,
foreign-key, and partition surfaces remain explicit failures.

## Compatibility Impact

`ENGINE=BLACKHOLE` moves from unsupported explicit engine request to partial
routed support. It covers the core application-visible behavior that rows are
accepted but not returned, and table metadata survives restart. It does not
claim native plugin parity for replication, full index capability, or binary-log
side effects.

`MEMORY` / `HEAP` remains planned because correct support needs a separate
volatile row-store lifecycle.

## File Lifecycle

No MariaDB BLACKHOLE data, index, or plugin files are created. Durable MyLite
state is limited to table metadata in the primary `.mylite` file; row writes do
not publish MyLite row pages or index-entry pages.

## Test And Verification Plan

- Add storage-engine smoke coverage for explicit `ENGINE=BLACKHOLE`.
- Verify requested/effective engine catalog metadata.
- Insert representative rows and assert full-scan and forced-index counts stay
  zero before and after close/reopen.
- Verify `SHOW CREATE TABLE` keeps `ENGINE=BLACKHOLE`.
- Keep `ENGINE=MEMORY` rejection coverage as a separate planned surface.
- Run the focused storage-smoke test, storage-engine compatibility report,
  formatting, shell, whitespace, and tidy checks.

## Acceptance Criteria

- `ENGINE=BLACKHOLE` creates a MyLite-routed table without durable sidecars.
- Inserted BLACKHOLE rows are not returned by full scans or supported index
  lookups.
- BLACKHOLE table metadata and `SHOW CREATE TABLE` survive close/reopen.
- `ENGINE=MEMORY` remains rejected until a volatile-storage design exists.
- Roadmap, compatibility, and architecture docs distinguish BLACKHOLE support
  from still-planned MEMORY/HEAP support.

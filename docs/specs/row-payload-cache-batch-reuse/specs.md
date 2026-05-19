# Row Payload Cache Batch Reuse

## Problem

Secondary-index cursor materialization already batches selected row ids and
uses a durable row-payload cache for repeated non-active reads. The hot loop
still rediscovers the same durable payload cache for every row id, including
repeated active-statement checks, filename comparisons, and header/table
fingerprint comparisons.

Local samples of repeated secondary exact reads showed this cache discovery
work under `mylite_storage_read_indexed_rows()` after page I/O and rowset
builder work had already been reduced.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::materialize_index_cursor_rows()`
  passes the selected row ids for a cursor to
  `mylite_storage_read_indexed_rows()` in one batch.
- `packages/mylite-storage/src/storage.c::mylite_storage_read_indexed_rows()`
  resolves the same filename/header/table row-payload cache on every row id
  through `append_cached_durable_row_payload_to_builder()`.
- `packages/mylite-storage/src/storage.c::read_row_ids_into_rowset()` uses the
  same helper for full rowset materialization after live-row collection.
- `packages/mylite-storage/src/storage.c::store_durable_row_payload()` resolves
  the same cache again when filling cache misses from the same batch.

## Design

- Resolve the durable row-payload cache once per row-id materialization batch
  after the header and table id are known.
- Pass a generation-guarded cache handle to the cache-hit builder helper so
  the hot path can reuse the pointer while the cache set is unchanged and
  re-resolve if the set moves or compacts.
- Fill cache misses through a helper that accepts the batch cache pointer by
  reference, creating or rotating the cache only when needed and refreshing the
  handle generation afterward.
- Preserve the existing non-active-only visibility rule: durable row-payload
  caches remain disabled when an active statement owned by any context or an
  active read snapshot can change visibility.
- Keep cache entries bounded by the existing cache-count and entry-count
  limits.

## Compatibility Impact

No SQL or API behavior changes. The cache is still keyed by the durable header
fingerprint and table id, and it is still disabled for active checkpoints and
read snapshots.

## Single-File And Lifecycle Impact

No file-format or companion-file change. The cache remains transient
thread-local memory and is still invalidated by the existing durable mutation
paths.

## Public API And File-Format Impact

No public API or file-format change.

## Storage-Engine Routing Impact

Durable routed tables benefit through existing MyLite handler index cursor
materialization, including omitted/default engine routing and `ENGINE=InnoDB`,
`ENGINE=MyISAM`, `ENGINE=Aria`, and `ENGINE=MYLITE`. Runtime-volatile
`MEMORY` / `HEAP` tables keep their separate volatile row path.

## Binary-Size And Dependency Impact

No new dependency and no meaningful binary-size impact.

## Test And Verification Plan

- Run storage unit tests for row payload, rowset, mutation invalidation, and
  recovery coverage.
- Rebuild storage-smoke targets and run the storage-engine compatibility
  harness.
- Run the local performance baseline to compare secondary exact-select
  timings.
- Run changed-line formatting checks and `git diff --check`.

## Acceptance Criteria

- Row-id batch materialization does not rediscover the same durable
  row-payload cache for every selected row while the cache set remains stable.
- Cache misses from the same batch can populate the already-selected cache.
- Existing storage and routed storage-engine compatibility tests pass.

## Risks

- This remains a transient row-payload cache, not a full pager. It reduces
  repeated cache-discovery overhead but does not remove MariaDB SQL execution,
  handler cursor, or page-cache costs that still separate MyLite from
  SQLite-like performance.

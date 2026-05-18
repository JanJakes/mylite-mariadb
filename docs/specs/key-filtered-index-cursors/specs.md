# Key-Filtered Index Cursors

## Problem

MyLite currently builds and sorts a complete in-memory cursor for every
supported index lookup. Recent slices made row materialization lazy and binary
searched the sorted cursor, but exact-key reads still pay the full cursor
construction cost before finding one row. This is visible in the local routed
storage performance baseline for direct and prepared primary-key point selects.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc::handler::ha_index_init()` records `INDEX` state and
  `active_index` after `index_init()` succeeds. It does not require the engine
  to position a cursor during `index_init()`.
- `mariadb/sql/handler.h::handler::index_read_map()` documents index reads as
  the positioning operation. `handler::ha_index_read_idx_map()` may bypass
  `index_init()` and asks the engine to position against an explicit key.
- `mariadb/storage/myisam/ha_myisam.cc::ha_myisam::index_init()` and
  `mariadb/storage/heap/ha_heap.cc::ha_heap::index_read_map()` keep initialization
  separate from key positioning. Their `index_read_map()` implementations pass
  the supplied key and find flag to the engine index search primitive.
- `mariadb/sql/handler.cc::handler::read_range_next()` trusts
  `index_next_same()` for equality ranges, so MyLite must preserve duplicate
  iteration after an exact-key read.

## Design

- Defer MyLite full cursor construction from `ha_mylite::index_init()` to the
  first operation that actually needs a full ordered cursor.
- Build filtered cursors for `HA_READ_KEY_EXACT` and `HA_READ_PREFIX` reads.
  The cursor still scans the append-only index-entry list for now, but it only
  retains matching entries and only sorts that matching subset.
- Stop after the first matching entry for non-nullable full-key unique lookups.
  Use raw byte equality only for full-key integer key images, where the
  MariaDB key image is safe for exact equality. Other key filters continue to
  use MariaDB key-tuple comparison so collation-sensitive string behavior stays
  authoritative.
- Preserve full cursor construction for full index scans, range reads that need
  neighboring keys, and `index_read_idx_map()` calls without a key.
- Track whether the current cursor is filtered. `index_first()` and
  `index_last()` rebuild a full cursor if the existing cursor is filtered,
  while `index_next()` and `index_next_same()` continue over the current cursor.
- Keep storage format and index-entry layout unchanged. This is a handler-layer
  performance slice, not a storage-level B-tree implementation.

## Compatibility Impact

Supported SQL behavior does not change. Exact and prefix reads must keep the
same duplicate, NULL, generated-column, prefix-index, and reopened-table
results as before. Range flags that may need the preceding or following key
continue to use a full cursor.

## Single-File And Lifecycle Impact

No file-format, sidecar, catalog, or embedded lifecycle behavior changes. The
slice only changes transient handler memory.

## Storage-Engine Routing Impact

The change applies to MyLite-routed tables regardless of requested engine name,
including routed `ENGINE=InnoDB`, `ENGINE=MyISAM`, and `ENGINE=Aria` tables.
`MEMORY` / `HEAP` volatile rows use the same handler cursor path.

## Binary-Size And Dependency Impact

No new dependencies. Binary-size impact should be negligible: one handler flag
and a small amount of conditional cursor construction logic.

## Tests And Verification

- Add regression coverage for an exact-key lookup followed by ordered duplicate
  iteration on a secondary key.
- Run the storage-engine smoke build and test.
- Run the storage-engine compatibility-harness group.
- Run the local performance baseline and record the machine-dependent result.
- Run changed-line `clang-format` checks and `git diff --check`.

## Acceptance Criteria

- `index_init()` no longer performs eager full cursor construction.
- Exact-key and prefix reads build a cursor that contains only matching entries.
- Full-key non-nullable unique integer lookups stop at the first matching
  entry.
- Full scans and range reads still see the complete ordered index.
- Existing routed storage compatibility tests pass.
- The local performance baseline is recorded in this spec or adjacent docs.

## Risks

- Filtering must not be applied to `HA_READ_KEY_OR_NEXT`,
  `HA_READ_KEY_OR_PREV`, `HA_READ_BEFORE_KEY`, `HA_READ_AFTER_KEY`, or
  `HA_READ_PREFIX_LAST_OR_PREV`, because those reads may need non-matching
  neighbor keys.
- Filtered cursors are still O(n) over index entries. Storage-level B-tree
  navigation remains the real long-term performance milestone.
- The local performance baseline is machine-dependent. The final run measured
  direct/prepared point selects at `2977.280` / `2998.540` us/op and
  direct/prepared updates at `9204.480` / `17752.660` us/op.

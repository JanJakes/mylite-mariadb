# Row-State Map Buckets

## Problem

The local storage performance baseline on 2026-05-19 still showed
SQLite-incompatible behavior after update-heavy workloads:

- direct primary-key updates in one transaction: `134.054 us/op`
- prepared primary-key updates in one transaction: `271.799 us/op`
- ordered full scan after 100k updates: `37471.707 us/op`

The scan cost comes from row-state visibility, not SQL semantics. Durable
updates and deletes append row-state pages that hide older row page ids.
`build_row_state_map()` collects those pages, but `find_row_state_entry()`
linearly scans the collected entries for every row/index candidate. A scan
after many updates therefore becomes O(candidate rows x row-state pages).

## Source References

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite storage rows and row-state pages are first-party code in
  `packages/mylite-storage/src/storage.c`.
- The affected paths are:
  - `scan_table_row_pages()`
  - `scan_exact_index_row_ids_from()`
  - `scan_exact_index_entries_from()`
  - `build_row_state_map()`
  - `find_row_state_entry()`

## Design

Keep the durable file format unchanged. Replace the in-memory row-state map's
linear lookup with transient hash buckets keyed by `source_row_id`.

- Build buckets as the map grows.
- Preserve latest-row-state-wins behavior when multiple state pages reference
  the same source row id.
- Grow entry and bucket storage geometrically instead of reallocating on every
  row-state page.
- Keep the map local to the read path. No cache lifetime, lock lifetime, or
  journal behavior changes.

## Compatibility

SQL-visible behavior is unchanged. Reads still apply the same row-state pages
and still ignore hidden row ids. Corruption checks remain in the existing page
decode paths.

## File Lifecycle

No file-format or companion-file change is introduced. The new buckets are
process-local heap memory freed with the row-state map.

## Verification

- Add storage coverage that creates enough update/delete row-state pages to
  grow the row-state buckets, then verifies full scans return only current rows
  in durable append order.
- Run storage unit tests.
- Run storage-engine smoke coverage.
- Run the local performance baseline and confirm the update-tail scan no longer
  scales with a linear row-state lookup per candidate row.

Local perf sample after implementation:

- direct primary-key updates in one transaction: `142.064 us/op`
- prepared primary-key updates in one transaction: `191.668 us/op`
- ordered full scan after 100k updates: `3898.697 us/op`

## Acceptance Criteria

- Row-state visibility lookup is near constant time for built maps.
- Existing update/delete, scan, exact-index, rollback, and recovery coverage
  passes.
- The performance baseline records a material reduction in the ordered full
  scan after many updates.

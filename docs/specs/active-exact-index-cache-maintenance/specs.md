# Active Exact Index Cache Maintenance

## Problem

After durable exact-index reads are cached, update statements inside an active
transaction still reload the active exact-index cache after every row mutation.
The local update-path sample showed `UPDATE ... WHERE id = ...` spending most
time in:

- `ha_mylite::build_index_cursor()`
- `mylite_storage_find_index_entry()`
- `find_cached_exact_index_entry()`
- `load_exact_index_cache()`
- `read_live_index_entries()`

The cache reload happens because update/delete invalidated the whole active
exact-index cache after each row-state publication.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/mylite-storage/src/storage.c::find_cached_exact_index_entry()`
  loads exact-index cache entries for active checkpoints and is used by
  duplicate checks and raw exact primary-key lookup during transactions.
- `mylite_storage_update_row_with_index_entries()` appends a replacement row,
  a row-state page hiding the old row id, and new index-entry pages.
- `mylite_storage_delete_row()` appends a row-state page hiding the old row id.
- Direct row reads already validate one row id by scanning row-state pages after
  that row id instead of rebuilding a full table row-state map.

## Design

- Maintain active exact-index caches incrementally on successful update/delete:
  - remove entries whose row id was hidden;
  - append replacement row entries for matching cached index/key-size pairs;
  - if an append to the cache cannot allocate memory, clear the active cache
    and fall back to the existing reload behavior on the next lookup.
- Keep durable exact-index caches invalidated on every durable mutation.
- Change update/delete row validation to use direct row-id visibility checks
  rather than rebuilding the full row-state map. A row id is live when the row
  page belongs to the target table and no later row-state page hides it.
- Keep truncate and catalog publications as full cache invalidation points.

## Compatibility Impact

No SQL-visible behavior should change. The cache mirrors rows and index entries
already published by the same storage mutation, and failures fall back to full
cache reloads.

## Single-File And Lifecycle Impact

No file-format change and no new companion files. This affects only transient
active-checkpoint memory and direct row-id validation reads.

## Test And Verification Plan

- Existing storage index-entry and transaction tests cover lookup behavior after
  update, delete, rollback, and savepoint rollback.
- Run storage unit tests.
- Rebuild storage-smoke targets and run the local performance baseline.
- Run the storage-engine compatibility harness.
- Run changed-line formatting checks and `git diff --check`.

## Acceptance Criteria

- Active exact-index caches remain correct across update and delete.
- Prepared and direct update timings improve materially in the local benchmark.
- Existing rollback and savepoint cache invalidation behavior remains covered.
- Routed storage compatibility tests pass.

## Risks

- This still does not make row-state visibility O(1). Updating older row ids
  still scans later row-state pages to confirm the candidate row is live.
- Full write performance still needs maintained index pages, row-state
  summaries, and pager work rather than append-only scans.

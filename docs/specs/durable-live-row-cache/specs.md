# Durable Live Row Cache

## Problem

After update-heavy workloads, full table reads still rebuild the live-row list
by walking every page in the append history. The current performance baseline
does a `COUNT(*)` verification after the update loops and then an ordered full
scan over the same unchanged table. Both paths call
`collect_live_table_row_ids()`, so the second read repeats row-page and
row-state-page checksums over the same durable checkpoint.

This is not the final SQLite-like storage shape. Maintained row directories,
navigable indexes, and pager/WAL work are still required. But repeated reads of
an unchanged durable checkpoint should not rederive the same live row-id list
inside one process.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::rnd_init()` materializes
  full-scan rows through `mylite_storage_read_rows()`.
- `packages/mylite-storage/src/storage.c::mylite_storage_read_rows()` reads
  the header, resolves the table id, calls `collect_live_table_row_ids()`, then
  materializes those row ids.
- `packages/mylite-storage/src/storage.c::mylite_storage_count_rows()` uses the
  same live-row collection path and validates the resulting row payloads before
  returning a count.
- `packages/mylite-storage/src/storage.c::collect_live_table_row_ids()` scans
  from page `2` to `header->page_count`, decodes row pages and row-state pages,
  builds a transient row-state map, and compacts hidden source row ids out of
  the live row-id list.
- Existing durable exact-index, row-payload, and index-leaf caches are
  thread-local and keyed by filename plus durable header fingerprint. They are
  disabled while active statements or read snapshots can mutate or pin a
  different view.

## Design

Add a small thread-local durable live-row cache keyed by filename, catalog root
page, catalog generation, page count, and table id. A cache entry owns a copy
of the compacted live row ids for one durable checkpoint.

Use the cache in:

- `mylite_storage_read_rows()`: copy cached row ids when available and
  materialize rows directly.
- `mylite_storage_count_rows()`: return the cached row-id count when available;
  otherwise collect and validate as today, then store the row ids.

Populate the cache only for non-active durable views and only when the row-id
count is within a bounded entry limit. Clear the cache alongside other durable
read caches on durable mutations, truncate, catalog invalidation, or cache-limit
rotation.

## Compatibility Impact

No SQL-visible behavior should change. The cache only reuses a live row-id list
derived from the same durable header fingerprint. Active statements,
transactions, and read snapshots continue to bypass durable caches.

## Single-File And Lifecycle Impact

No file-format, journal, WAL, lock, or companion-file changes. The cache is
transient process memory and is discarded on invalidation, process exit, or
cache-limit rotation.

## Storage-Engine Routing Impact

The cache applies only to durable MyLite-routed tables. Volatile `MEMORY` /
`HEAP` rows continue through the volatile storage path.

## Binary-Size Impact

Expected to be negligible: a small first-party cache structure with no new
dependencies.

## Test And Verification Plan

- Add storage-level coverage that reads/counts rows, mutates the table, and
  verifies later reads/counts observe the mutation rather than a stale cached
  row list.
- Build storage-smoke targets.
- Run focused storage and embedded storage-engine smoke tests.
- Run the full storage-smoke CTest suite.
- Run the local performance baseline at small and higher iteration counts.
- Run `git diff --check`.

## Acceptance Criteria

- Repeated durable full-row/count reads over the same header can reuse cached
  live row ids.
- Mutations and catalog/truncate invalidations clear or bypass stale cache
  entries.
- Active checkpoints and snapshots do not use durable live-row caches.
- Full-scan performance after unchanged count/read probes improves in the local
  performance baseline.

## Risks

- Returning stale row ids would expose deleted or superseded rows. The header
  page-count fingerprint and mutation invalidation must stay aligned.
- The cache can hide external same-process file corruption until invalidation,
  like the existing durable row-payload and exact-index caches. This is an
  accepted tradeoff for transient same-process hot reads, not a durability
  guarantee.
- This still does not solve first-read full-scan cost after a large append
  history. That requires maintained row directories or compaction.

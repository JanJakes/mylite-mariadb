# Durable Exact Index Read Cache

## Problem

After handler row stats stopped scanning rows during SELECT optimization, the
next sampled read bottleneck is exact index lookup over indexes without a
published leaf root. Each lookup rescans and checksums the append-only index
page history:

- `ha_mylite::build_index_cursor()`
- `mylite_storage_read_exact_index_entries()` or
  `mylite_storage_find_index_entry()`
- `scan_exact_index_entries_from()`
- `decode_index_entry_page()`
- `checksum_page()`

That keeps ordinary primary-key point reads and initial `CREATE TABLE` secondary
indexes around tens of milliseconds per lookup in the local routed-storage
benchmark, even after optimizer stats are cheap.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::build_index_cursor()`
  uses storage exact lookup APIs for full non-nullable raw key predicates.
- `packages/mylite-storage/src/storage.c::mylite_storage_find_index_entry()`
  already has an active-checkpoint exact-index cache for duplicate checks inside
  write statements. That cache is deliberately tied to active statement state.
- `packages/mylite-storage/src/storage.c::mylite_storage_read_exact_index_entries()`
  uses a published leaf root when available, then falls back to append-log exact
  scans.
- Existing storage unit coverage checks exact lookup and exact entryset results
  across insert, update, delete, truncate, transaction, and savepoint paths.

## Design

Add a small thread-local durable exact-index read cache for storage calls made
outside active MyLite storage statements:

- cache scope is process/thread local, filename-specific, and tied to the
  observed header fingerprint (`catalog_root_page`, `catalog_generation`, and
  `page_count`);
- each cache stores all live entries for one table id, index number, and key
  size, reusing the existing exact-index cache payload shape;
- `mylite_storage_find_index_entry()` uses the durable cache after checking for
  an active statement cache and after trying a published leaf root;
- `mylite_storage_read_exact_index_entries()` uses the durable cache only when
  no published leaf root was used, so leaf-backed reads keep their targeted page
  search path;
- any successful write, row-state publication, or catalog publication clears
  durable caches for the affected primary file;
- active statements and transaction snapshots do not use the durable cache,
  preserving current checkpoint visibility rules.

This is not a replacement for maintained B-tree pages. It amortizes repeated
exact lookups over the current append-only format until the pager/index work
lands.

## Compatibility Impact

No SQL-visible behavior should change. The cache only stores results that the
existing exact scan would have returned for the same header state.

## Single-File And Lifecycle Impact

No file-format change and no new companion files. The cache is transient
process memory and is invalidated on primary-file mutations.

## Public API And File-Format Impact

No public API or file-format change.

## Storage-Engine Routing Impact

Durable MyLite-routed tables benefit when repeated exact lookups hit indexes
without a published leaf root. Volatile MEMORY/HEAP paths keep their separate
in-memory implementation.

## Test And Verification Plan

- Rely on existing storage index-entry tests to catch stale cache results across
  update and delete after prior lookups.
- Run storage unit tests and the storage-engine compatibility harness.
- Run the local performance baseline at `1000 50` or larger and compare
  primary-key and unpublished secondary exact reads.
- Run changed-line formatting checks and `git diff --check`.

## Acceptance Criteria

- Published-leaf exact reads continue using leaf lookup rather than loading the
  durable cache.
- Repeated primary-key and unpublished secondary exact reads improve materially
  in the local benchmark.
- Update/delete/truncate and catalog mutations cannot return stale cached exact
  lookup results.
- Routed storage compatibility tests pass.

## Risks

- A process-local cache can grow if many files and indexes are touched. Keep the
  first implementation small and clear old caches when the fixed cache-count
  budget is reached.
- This does not address first-lookup cost, single-shot lookup performance, or
  write-side index maintenance. Those still require maintained index pages and
  pager work.

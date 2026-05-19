# Durable Index Leaf Page Cache

## Problem

After durable row payload cache lookups became near constant time, the local
sample showed published leaf-root exact reads spending most storage-local time
in repeated index leaf page reads, checksums, and decode validation. The same
small leaf run is read for each secondary cursor build in the benchmark.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::build_index_cursor()` calls
  `mylite_storage_read_exact_index_entries()` for secondary exact cursors.
- `packages/mylite-storage/src/storage.c::read_index_leaf_exact_entries()`
  prefers catalog-published leaf roots when available.
- `read_index_leaf_page()` currently reads, checksums, and decodes each leaf
  page every time the cursor probes a published root.

## Design

- Add a bounded thread-local durable index-leaf page cache keyed by filename,
  catalog root, catalog generation, page count, and page id.
- Use the cache only for non-active reads. Active statements and read snapshots
  keep their existing file/snapshot semantics.
- Populate the cache after a successful `read_index_leaf_page()` decode and
  copy cached raw page bytes plus decoded metadata back to the caller on
  subsequent reads.
- Clear the leaf page cache through the durable exact-index invalidation path
  so append, update, delete, truncate, and catalog publications cannot reuse
  stale leaf pages.
- Bound cache count and entries to keep the optimization transient and
  predictable until a real pager owns this behavior.

## Compatibility Impact

SQL-visible behavior is unchanged. Cached leaf pages are used only when the
file header fingerprint is identical and no active checkpoint or snapshot
changes visibility rules.

## Single-File And Lifecycle Impact

No file-format change and no new companion files. The cache is thread-local
memory and is invalidated by durable mutations.

## Test And Verification Plan

- Existing storage and routed-engine tests cover index leaf correctness,
  mutation invalidation, and snapshot behavior.
- Rebuild storage and storage-smoke targets.
- Run storage unit tests and the storage-engine compatibility harness.
- Run the local performance baseline to measure published leaf-root exact-read
  impact.
- Run changed-line formatting checks and `git diff --check`.

## Acceptance Criteria

- Repeated durable published leaf-root exact cursor reads avoid repeated leaf
  page reads, checksums, and decode validation for cached pages.
- Existing storage and routed-engine compatibility tests pass.
- Published leaf-root exact-select timings improve materially in the local
  benchmark.

## Risks

- This is still not a full pager: it caches decoded index leaf pages only for
  non-active durable reads. A real pager remains necessary for broad
  SQLite-like behavior.

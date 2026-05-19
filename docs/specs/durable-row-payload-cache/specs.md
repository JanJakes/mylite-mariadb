# Durable Row Payload Cache

## Problem

After index cursor row materialization and live-row validation caching, the
local sample showed secondary exact-select time dominated by repeated row page
reads and checksums inside `mylite_storage_read_indexed_rows()`. The benchmark
reuses a small set of secondary keys, so the same durable row payload pages were
decoded again for each cursor build.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::materialize_index_cursor_rows()` calls
  `mylite_storage_read_indexed_rows()` for durable index cursors.
- `packages/mylite-storage/src/storage.c::mylite_storage_read_indexed_rows()`
  reads and checksums every requested row page, even when the same durable
  header/table/row id was read by a previous non-active statement.
- Existing durable exact-index caches are already invalidated on writes and are
  disabled while active statements or snapshots own the file.

## Design

- Add a bounded thread-local durable row-payload cache keyed by filename,
  catalog root, catalog generation, page count, table id, and row id.
- Give each cache a small open-addressed row-id index so repeated hits do not
  turn the bounded cache into a linear per-row scan.
- Use the cache only for non-active reads. Active statements and read snapshots
  keep their existing file/snapshot semantics.
- Populate the cache from successful `mylite_storage_read_indexed_rows()` row
  page reads and append cached payloads to the caller's rowset on subsequent
  reads.
- Clear the payload cache through the durable exact-index invalidation path so
  append, update, delete, truncate, and catalog publications cannot reuse stale
  payloads.
- Bound cache count, entries, and hash buckets to keep the optimization
  transient and predictable until a real pager owns this behavior.

## Compatibility Impact

SQL-visible behavior is unchanged. Cached payloads are used only when the file
header fingerprint is identical and no active checkpoint or snapshot changes
visibility rules.

## Single-File And Lifecycle Impact

No file-format change and no new companion files. The cache is thread-local
memory and is invalidated by durable mutations.

## Test And Verification Plan

- Existing storage and routed-engine tests cover row payload correctness,
  mutation invalidation, and snapshot behavior.
- Rebuild storage and storage-smoke targets.
- Run storage unit tests and the storage-engine compatibility harness.
- Run the local performance baseline to measure secondary exact-select impact.
- Run changed-line formatting checks and `git diff --check`.

## Acceptance Criteria

- Repeated durable secondary exact-select cursor materialization avoids
  repeated row page checksums and linear row-payload cache scans for cached row
  ids.
- Existing storage and routed-engine compatibility tests pass.
- Secondary exact-select timings improve materially in the local benchmark.

## Risks

- This is still not a full pager: it caches row payloads, not arbitrary decoded
  pages, and it is intentionally disabled for active snapshots. A real pager
  remains necessary for broad SQLite-like behavior.

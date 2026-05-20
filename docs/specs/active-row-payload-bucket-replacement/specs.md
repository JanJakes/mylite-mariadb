# Active Row Payload Bucket Replacement

## Problem

Update-path sampling after read-startup reductions shows
`replace_active_row_payload()` spending most of its time rebuilding row-payload
cache buckets after each successful replacement update. The active row-payload
cache is already useful because handler update loops can reuse row images that
were materialized through indexed cursors, but the current replacement path
turns each cached row-id change into a full cache-bucket rebuild.

That O(n) rebuild cost is avoidable. The cache uses open-addressed buckets and
can update one old row id to one new row id without reallocating or reinserting
the whole cache.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc::handler::ha_update_row()` validates constraints,
  marks the table transaction read-write, then dispatches to the engine
  `update_row()` hook.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::update_row()` prepares the
  replacement row payload and calls
  `mylite_storage_update_row_with_index_entries()`.
- `packages/mylite-storage/src/storage.c::mylite_storage_update_row_with_index_entries()`
  appends the replacement row, replacement row-state, and replacement index
  entries, then maintains active exact-index, live-row, live-row-id, and
  row-payload caches after successful publication.
- `replace_active_row_payload()` calls
  `replace_row_payload_cache_entry()`.
- `replace_row_payload_cache_entry()` currently finds the old entry, replaces
  the payload, changes the row id, and calls
  `rebuild_row_payload_cache_buckets()` whenever buckets exist.
- `remove_row_payload_cache_entry()` also compacts the entry array and rebuilds
  all buckets after deletes.

## Design

Keep the existing row-payload cache entry array and bucket table, but make
bucket deletion tombstone-aware:

- bucket states become empty, occupied, or deleted;
- lookups probe through deleted buckets and stop at empty buckets;
- inserts can reuse the first deleted bucket seen on their probe path;
- row-id replacement inserts the new row-id bucket for the same entry index,
  marks the old row-id bucket deleted, then updates the entry payload and row
  id;
- row removal marks the removed row-id bucket deleted, swap-removes the entry
  from the array, and retargets the moved entry's bucket to the new entry
  index;
- accumulated tombstones trigger an opportunistic same-capacity rebuild only
  after they outnumber live entries.

The cache remains best-effort. Allocation failure while replacing a cached
payload keeps the existing fallback: the caller removes the stale cached old
row and continues with correct storage behavior.

## Compatibility Impact

No SQL, public C API, handler, or optimizer behavior changes. This only changes
how a transient storage cache indexes row payloads already visible to the
active MyLite checkpoint.

## Single-File And Lifecycle Impact

No file-format, journal, WAL, lock, or companion-file changes. The affected
state is transient process memory owned by active statements and durable read
caches.

## Storage-Engine Routing Impact

The improvement applies to durable MyLite-routed tables, including tables
requested as `MYLITE`, `InnoDB`, `MyISAM`, `Aria`, and omitted/default engines
that route to MyLite. `MEMORY` / `HEAP` volatile rows are unaffected.

## Binary-Size And Dependency Impact

Small first-party C change. No new dependency and no measurable binary-size
expectation beyond normal code-size noise.

## Tests And Verification

- Add storage coverage for a transaction that materializes many indexed row
  payloads, replaces them with new row ids, verifies old keys disappear, then
  deletes cached rows.
- Reuse existing active row-payload, savepoint rollback, durable row-payload,
  exact-index, and embedded storage-engine smoke coverage.
- Run the storage-smoke build targets and full CTest suite.
- Run the update-only performance baseline before and after the change.
- Run `git diff --check` and `git clang-format --diff`.

## Acceptance Criteria

- Active row-payload replacement no longer rebuilds the full bucket table for
  each cached update.
- Row-payload lookup remains correct after many replacements, tombstone reuse,
  entry swap-removal, savepoint rollback, and delete.
- Existing storage and embedded storage-engine tests pass.
- Update-path performance improves or the profile no longer shows per-update
  row-payload bucket rebuild as the dominant active-cache cost.

## Risks And Open Questions

- Open-addressed deletion is easy to make subtly wrong. Tombstone lookups and
  periodic rebuilds are safer here than backward-shift deletion because the
  current cache has low load factor and small bounded capacity.
- Tombstones can make miss probes longer if left indefinitely. The rebuild
  threshold keeps this bounded without reintroducing per-update rebuild cost.

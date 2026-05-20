# Exact Index Cache Incremental Maintenance

## Problem

After row-payload cache bucket replacement, update-path sampling shows the next
avoidable active-cache cost in exact-index cache maintenance. Successful
updates remove the old row id from each active exact-index cache, append the
new key/row-id entry, clear lookup buckets, and force the next duplicate-key or
point-lookup probe to rebuild the full bucket table.

For hot update loops over a bounded active transaction, the exact-index cache
already contains the visible index entries. Rebuilding lookup buckets and
memmoving key arrays for each row-id replacement is unnecessary.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc::handler::ha_update_row()` dispatches update
  execution to the storage-engine `update_row()` hook after constraint checks.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::update_row()` checks
  duplicate keys, prepares the new row payload and index entries, and calls
  `mylite_storage_update_row_with_index_entries()`.
- `packages/mylite-storage/src/storage.c::replace_active_exact_index_cache_entries()`
  maintains active exact-index caches after successful replacement updates.
- `remove_exact_index_cache_entries_by_row_id()` currently compacts key and
  row-id arrays with per-entry `memmove()` and clears all lookup buckets.
- `append_exact_index_cache_entry()` appends the replacement entry and clears
  all lookup buckets again.
- `find_exact_index_cache_entry_row_id()` and
  `append_exact_index_cache_matches_to_entryset()` rebuild buckets lazily on
  the next lookup via `ensure_exact_index_cache_buckets()`.

## Design

Make exact-index cache entries tombstone-aware and maintain buckets
incrementally when buckets are already valid:

- track live entry count and dead entry count separately from allocated entry
  slots;
- mark removed row-id entries dead instead of compacting arrays on every
  update/delete;
- unlink removed live entries from their bucket chain when buckets are valid;
- append replacement entries to the existing bucket chain when buckets are
  valid and the bucket table is still large enough for the live count;
- allocate `bucket_next` for entry capacity, not only current count, so appends
  can link new entries without rebuilding buckets;
- compact dead entries in order only when tombstones outnumber live entries,
  then rebuild buckets if they were valid.

This preserves the existing exact-entry scan order for live entries while
amortizing compaction across many updates. Allocation failures remain
best-effort: if incremental maintenance cannot preserve a cache, callers keep
the existing behavior of clearing active exact-index caches.

## Compatibility Impact

No SQL, public C API, handler, or optimizer behavior changes. Exact-index
caches are transient acceleration structures over already-visible MyLite index
entries.

## Single-File And Lifecycle Impact

No file-format, journal, WAL, lock, or companion-file change. The affected
state is process-local active/durable cache memory.

## Storage-Engine Routing Impact

The improvement applies to durable MyLite-routed tables, including requested
`MYLITE`, `InnoDB`, `MyISAM`, `Aria`, and omitted/default engines routed to
MyLite. Runtime-volatile `MEMORY` / `HEAP` rows are unaffected.

## Binary-Size And Dependency Impact

Small first-party C change. No new dependency.

## Tests And Verification

- Add storage coverage for active exact-index cache replacements over many rows
  and duplicate-key probes, including update, delete, and replacement lookup
  after bucket maintenance.
- Reuse existing index-entry, active exact-index, savepoint, storage, and
  embedded storage-engine tests.
- Run the storage-smoke build targets and full CTest suite.
- Run the update performance baseline and compare the exact-cache profile.
- Run `git diff --check` and `git clang-format --diff`.

## Acceptance Criteria

- Active exact-index cache updates do not clear and rebuild lookup buckets for
  every row replacement when buckets are already valid.
- Exact-index lookup and entryset order remain correct after many replacements,
  deletes, compaction, rollback, and cache promotion.
- Existing storage and embedded storage-engine tests pass.
- Update-path profile no longer shows `ensure_exact_index_cache_buckets()` as a
  dominant per-update cost caused by active cache replacement.

## Risks And Open Questions

- Tombstone compaction must preserve live entry order because entryset reads can
  expose storage iteration order.
- Incremental bucket maintenance must not leave dead entries reachable through
  bucket chains.
- This does not address the remaining write syscall cost. SQLite-like write
  performance still needs pager/WAL work after cache maintenance is no longer
  the main cost.

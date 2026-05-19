# Active Exact Index Cache Promotion

## Problem

Active exact-index caches are maintained across row mutations once loaded, but
the first exact lookup in a new active transaction still rebuilds the cache by
scanning live index-entry pages. After large update transactions, the next
transaction pays that scan again even though the previous active cache held a
complete maintained exact-index view at commit.

Sampling after nested checkpoint cloning shows the remaining prepared-update
startup hotspot in:

- `find_cached_exact_index_entry()`
- `load_exact_index_cache()`
- `read_live_index_entries()`
- `decode_index_entry_page()`
- `checksum_page()`

## Source Findings

- `find_cached_exact_index_entry()` loads active exact-index caches from live
  index-entry pages when the active statement has no matching cache.
- `replace_active_exact_index_cache_entries()` maintains already-loaded active
  exact-index caches across update/delete mutations.
- Durable exact-index caches are keyed by filename, catalog root page, catalog
  generation, page count, table id, index number, and key size, and are disabled
  while active checkpoints exist.
- Top-level `mylite_storage_commit_statement()` is the boundary where a
  maintained active cache can become durable process-local read state.

## Design

When an active exact-index cache is missing, try to seed it from a matching
durable exact-index cache for the active checkpoint header before scanning the
index-entry history. If no durable cache matches, keep the existing load path.

On top-level commit, promote maintained active exact-index caches to durable
exact-index caches under the committed header fingerprint. Any rollback,
catalog change, truncate, allocation failure during maintenance, or explicit
cache invalidation keeps the existing clear-and-reload behavior.

## Compatibility Impact

No SQL-visible behavior should change. The active cache is seeded only from a
complete durable cache for the same checkpoint and is already maintained by the
existing mutation hooks.

## Single-File And Lifecycle Impact

No file-format or companion-file change. Caches remain transient process memory.

## Test And Verification Plan

- Rely on existing exact-index, update/delete, transaction, savepoint rollback,
  and storage-engine tests.
- Build storage-smoke targets.
- Run focused storage and embedded storage-engine smoke tests.
- Run the full storage-smoke CTest suite.
- Run the local performance baseline at small and higher iteration counts.
- Run `git diff --check`.

## Acceptance Criteria

- Active exact-index cache loads prefer a matching durable complete cache.
- Top-level commit publishes maintained active exact-index caches under the
  final durable header fingerprint.
- Rollback and invalidation paths cannot expose stale exact-index entries.
- The local performance baseline reduces repeated transaction-to-transaction
  exact-index cache reload cost.

## Risks

- Promoting a partially maintained cache would be incorrect. The existing
  mutation hooks clear active caches on maintenance uncertainty, so promotion
  must only copy currently live statement caches.
- This still does not remove first-lookup cost after cold open. Durable index
  pages and pager work remain required for SQLite-like cold performance.

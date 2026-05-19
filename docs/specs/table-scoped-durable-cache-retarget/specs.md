# Table-Scoped Durable Cache Retarget

## Problem

Durable process-local caches are keyed by the committed header fingerprint,
including the global page count. A row mutation on any table advances that page
count and the current mutation path clears all durable row/index caches for the
file. That is safe, but it discards caches for unrelated tables whose live rows,
row payloads, and exact-index entries did not change.

The local performance baseline exposes this after the `perf_rows` table is
read, a separate `perf_prepared_rows` table is populated, and later updates on
`perf_rows` start without the durable live-row-id seed that would avoid a
post-commit append-history scan.

## Source Findings

- `mylite_storage_write_row()`, `mylite_storage_update_row_with_index_entries()`,
  and `mylite_storage_delete_row()` know the mutated `table_id` and the final
  committed header after the row mutation publishes.
- Durable live-row-id, row-payload, and exact-index caches all carry `table_id`
  plus the header fingerprint.
- Durable index-leaf page caches are keyed by the file header fingerprint and
  contain immutable published leaf-page bytes. Row DML appends visibility tail
  pages but does not rewrite the published leaf pages.
- Catalog changes, truncate, rollback, and cache-limit rotation still need the
  existing broad invalidation behavior.

## Design

After successful row insert, update, or delete, clear durable caches for the
mutated table, but retarget durable caches for other tables in the same file to
the new header fingerprint. Retargeting updates catalog root, catalog
generation, and page count metadata; it does not alter cached row ids, payloads,
or exact-index entries.

Keep the existing full-file invalidation for catalog changes, truncate,
rollback recovery, and cache-limit overflow. Clear the cached read file on any
row mutation so future reads still reopen or revalidate the primary file handle.

## Compatibility Impact

No SQL-visible behavior changes. The retained caches are table-local and remain
valid only for tables whose row/index state did not change.

## Single-File And Lifecycle Impact

No file-format or companion-file change. The cache retargeting is transient
process memory only and remains guarded by the current durable header
fingerprint.

## Test And Verification Plan

- Build storage-smoke targets.
- Run focused storage and embedded storage-engine smoke tests.
- Run the full storage-smoke CTest suite.
- Run the local performance baseline at small and higher iteration counts.
- Run `git diff --check`.

## Acceptance Criteria

- Same-table row mutations clear stale table-local durable caches.
- Unrelated table row mutations keep durable table-local caches usable under the
  new committed page count.
- Catalog, truncate, rollback, and cache-limit paths retain broad invalidation.
- The local performance baseline avoids the post-update count scan that
  previously re-read and rechecksummed the append history after unrelated table
  mutations had stranded the live-row cache.

## Risks

- Retargeting a cache for a table that actually changed would be incorrect, so
  row mutation paths must call the retarget helper with the exact mutated table
  id after the final header is published.
- Published index-leaf page bytes are retained across row DML because row DML
  appends tail visibility pages rather than modifying leaf pages. If a future
  slice rewrites published leaves during DML, it must narrow or remove that
  retargeting path.

# Active Row Payload Cache

## Problem

Repeated indexed updates inside one active MyLite transaction still materialize
the current row payload from the primary file before each handler update. After
the recent live-row validation and coalesced write-path slices, the remaining
prepared-update profile still shows row-page reads, row-page checksums, and
payload validation under the MariaDB index cursor path.

The row payload is already known after the first indexed read, and a cached row
payload can be updated in place after each successful replacement update.
Re-reading and re-checksumming that same current row image inside the same
active checkpoint is avoidable.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_update.cc::Sql_cmd_update::update_single_table()` calls
  `table->file->ha_update_row(table->record[1], table->record[0])` after the
  matching old row has been read into `record[1]`.
- `mariadb/sql/handler.cc::handler::ha_update_row()` asserts the handler has a
  write lock, marks the transaction as read-write, then dispatches to the
  engine `update_row()`.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::index_read_map()` builds a
  MyLite index cursor for exact primary-key updates.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::read_index_cursor_row()`
  materializes the current row payload through
  `mylite_storage_read_indexed_row()` when the cursor row has not been
  pre-materialized.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::update_row()` writes the
  replacement through `mylite_storage_update_row_with_index_entries()`.
- `packages/mylite-storage/src/storage.c::read_indexed_row_payload_from_open_file()`
  already uses the durable row-payload cache for non-active reads, but durable
  caches are deliberately disabled while an active statement or transaction can
  mutate the file.
- `packages/mylite-storage/src/storage.c::replace_active_live_row()` and
  `replace_active_exact_index_cache_entries()` already maintain transaction
  local row visibility and exact-index state after successful replacement
  updates.

## Design

Add a transaction/statement-local row-payload cache to
`mylite_storage_statement`. The cache is keyed by filename, table id, catalog
root page, catalog generation, and row id, but unlike the durable cache it does
not use `page_count` as a stale-view guard because active checkpoints append
pages continuously while preserving their own view.

The cache is populated when an active indexed-row read successfully materializes
a row payload. On successful update, replace the cached old row id with the new
row id and replacement payload when the old row was cached. On successful
delete, remove the old row id. On truncate or catalog-affecting mutations,
clear active row-payload caches for the statement chain.

The active cache is best-effort: allocation failure disables the optimization
for that row and must not change SQL-visible behavior. The durable payload cache
remains unchanged and remains disabled while active checkpoints exist.

## Compatibility Impact

No SQL, C API, handler, or optimizer behavior should change. The cache only
avoids repeat row-page validation for row payloads that the active checkpoint
has already materialized.

## Single-File And Lifecycle Impact

No file-format, journal, WAL, lock, or companion-file changes. Cached payloads
are transient process memory owned by the active statement/transaction and are
freed on commit, rollback, statement cleanup, or cache invalidation.

## Storage-Engine Routing Impact

The change applies only to durable MyLite-routed rows. `MEMORY` / `HEAP`
volatile rows continue through the volatile storage path.

## Binary-Size Impact

Expected to be negligible: this reuses the existing row-payload cache entry and
bucket implementation with a small statement-owned cache set.

## Test And Verification Plan

- Add storage-level coverage that:
  - reads a row through an active indexed lookup,
  - updates it and proves the old key disappears while the new key returns the
    replacement payload,
  - rolls back a savepoint after another cached update and proves the parent
    payload remains correct,
  - deletes a cached row and proves the cached payload does not survive.
- Build storage-smoke targets.
- Run the focused storage and embedded storage-engine smoke tests.
- Run the full storage-smoke CTest suite.
- Run the local performance baseline at small and higher iteration counts.
- Run `git diff --check`.

## Acceptance Criteria

- Active indexed-row reads can return a transaction-local cached row payload.
- Successful active update/delete paths maintain or remove already-cached
  payloads.
- Savepoint rollback and truncate/catalog invalidation do not expose stale row
  payloads.
- Existing durable row-payload caches remain disabled for active checkpoints.
- The local performance baseline shows lower repeated update cost or, if
  noise hides the effect, profile evidence no longer shows repeated row-page
  payload reads as the same hot spot.

## Risks

- A stale active row payload would corrupt MariaDB's old-row image before
  `ha_update_row()`. The implementation must treat rollback, delete, truncate,
  and catalog writes as invalidation points.
- BLOB/TEXT rows carry storage-owned value bytes in MyLite row payloads. The
  cache may store those durable payload bytes, but must return copies so handler
  row buffers own their normal lifetimes.
- This is still not a pager. SQLite-like performance still needs maintained
  navigable indexes, page-cache/WAL work, and fewer full-page checksums on hot
  mutation paths.

# Active Live Row List Maintenance

## Problem

The durable live-row cache avoids repeated full append-history scans for an
unchanged checkpoint, but the first read after a large active transaction still
has to rebuild the live row-id list from every historical row and row-state
page. Update-heavy transactions often preserve row count and only replace row
ids. When a complete durable live-row list exists before such a transaction,
MyLite can maintain that list through active mutations and publish the updated
list on commit.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::external_lock()` begins a MyLite
  storage transaction checkpoint for explicit SQL transactions before row DML.
- `packages/mylite-storage/src/storage.c::mylite_storage_update_row_with_index_entries()`,
  `mylite_storage_delete_row()`, and
  `mylite_storage_append_row_with_index_entries()` already maintain active
  exact-index, live-row validation, and row-payload caches after successful
  mutations.
- `packages/mylite-storage/src/storage.c::mylite_storage_commit_statement()`
  is the checkpoint boundary where top-level active state can become a durable
  read cache.
- The durable live-row cache is keyed by filename, catalog root page, catalog
  generation, page count, and table id, and is disabled while active
  checkpoints exist.

## Design

Add a statement-owned complete live-row list cache. It is seeded only from an
existing durable live-row cache for the same table and checkpoint. If no
complete durable list exists, active mutations keep using the current scan
fallbacks and no complete-list claim is made.

For seeded active lists:

- inserts append the new row id,
- updates replace the old row id with the replacement row id,
- deletes remove the old row id,
- truncate, catalog changes, unsupported overflow of the bounded list, or
  unexpected missing row ids clear the active complete-list cache.

On top-level commit, promote any complete active lists to durable live-row
caches under the committed header fingerprint. On rollback, discard them.
Nested savepoint rollback clears parent complete-list caches, matching the
existing exact-index/live-row cache invalidation rule.

## Compatibility Impact

No SQL-visible behavior should change. The cache is used only as a complete
row-id list when it was seeded from a complete durable checkpoint list and all
subsequent active mutations were maintained successfully.

## Single-File And Lifecycle Impact

No file-format, journal, WAL, lock, or companion-file changes. The cache is
transient process memory and is promoted only after a successful top-level
checkpoint commit.

## Storage-Engine Routing Impact

The change applies only to durable MyLite-routed tables. Volatile
`MEMORY` / `HEAP` tables continue through volatile storage.

## Binary-Size Impact

Expected to be negligible: this reuses the durable live-row cache structure for
statement-owned state.

## Test And Verification Plan

- Add storage-level coverage that seeds the durable live-row cache, performs an
  active transaction with update, delete, and insert, commits it, and verifies
  later count/read results match the committed state.
- Keep rollback/savepoint behavior covered through cache clearing on nested
  rollback.
- Build storage-smoke targets.
- Run focused storage and embedded storage-engine smoke tests.
- Run the full storage-smoke CTest suite.
- Run the local performance baseline at small and higher iteration counts.
- Run `git diff --check`.

## Acceptance Criteria

- Active complete live-row lists are seeded only from complete durable cached
  lists.
- Successful active insert/update/delete maintains seeded lists.
- Commit promotes maintained lists to durable cache entries under the final
  header fingerprint.
- Rollback, truncate, catalog changes, cache overflow, or inconsistent mutation
  state discards the active list instead of risking stale rows.
- The local performance baseline reduces the first post-update count/full-scan
  append-history walk when a pre-transaction live-row cache exists.

## Risks

- A partially maintained list would be worse than no cache. Any uncertainty
  must clear the active complete-list cache and fall back to scanning.
- This is still transient state. It does not replace the planned maintained row
  directory, compaction, pager, or WAL work needed for durable first-read
  performance after cold open.

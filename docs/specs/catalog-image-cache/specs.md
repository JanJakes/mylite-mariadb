# Catalog Image Cache

## Problem

The catalog now grows as an append-only page chain. The existing active
statement and read checkpoint cache still remembers only the validated catalog
root page, so metadata reads over a multi-page catalog can repeatedly reread
and revalidate overflow pages even when the same header root page and catalog
generation are already pinned by an active statement or read statement.

That is a regression risk for table discovery, catalog-backed DDL metadata, and
metadata-heavy prepared execution loops. The cache should match the new
catalog unit: a validated catalog image, not only page 1 or the current root.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.h` documents that engine
  `discover_table_names()` should be implemented when `discover_table()` would
  be slow for existence/name discovery. MyLite implements table discovery
  callbacks in `mariadb/storage/mylite/ha_mylite.cc`.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_discover_table()`,
  `mylite_discover_table_names()`, and
  `mylite_discover_table_existence()` call the storage catalog through
  `mylite_storage_read_table_definition()`, `mylite_storage_list_tables()`,
  and `mylite_storage_table_exists()`.
- `packages/mylite-storage/src/storage.c::read_checkpoint_snapshot()` and the
  active catalog-root cache validate and retain the current root page for a
  read statement or active write statement.
- `packages/mylite-storage/src/storage.c::read_catalog_image()` now builds an
  owned contiguous catalog image by reading the validated root and following
  contiguous `next_page` links.

## Design

- Add a transient catalog-image cache to storage statements, keyed by the
  active catalog root page id and catalog generation.
- Populate the cache when a read checkpoint snapshot is opened. The snapshot
  already owns a stable header/root view, so it can validate the full catalog
  chain once and serve later catalog reads from memory.
- Let `read_catalog_image()` return a copy of a matching cached image before
  touching the file. Writers still receive a mutable owned copy, preserving the
  current append/remove/rename helper contracts.
- Cache newly read catalog images for active statements, read statements, and
  matching read checkpoint cache entries when the cache copy can be allocated.
  Cache allocation failure must not make a successful catalog read fail.
- Keep the rollback journal shape unchanged. The existing root-page snapshot is
  still sufficient for rollback because overflow pages in committed chains are
  immutable.
- Invalidate both the root-page cache and the catalog-image cache when catalog
  root page or generation changes.

## Compatibility Impact

SQL-visible behavior is unchanged. The cache stores only catalog images that
have passed the same page, checksum, generation, and record validation as the
uncached path.

## DDL Metadata Routing Impact

Routed schema, table, foreign-key, and index-root metadata reads can reuse the
same validated catalog image while the statement snapshot remains current.
Catalog DDL continues to publish a fresh chain and invalidate stale metadata
caches through the generation/root-page key.

## Single-File And Lifecycle Impact

No file-format change and no new sidecar. The cache is transient process memory
owned by statement and read-checkpoint cache lifetimes.

## Public API And File-Format Impact

No public C API change. Existing single-page and multi-page catalog files
remain valid.

## Storage-Engine Routing Impact

Engine-neutral routed metadata benefits because all supported engine spellings
resolve through the same MyLite catalog helpers.

## Wire-Protocol And Integration Impact

No wire-protocol package change. Integration layers observe the same metadata
through faster catalog reads.

## Binary-Size And Dependency Impact

No new dependency. Binary impact is limited to catalog-image cache ownership,
copying, and invalidation helpers plus tests.

## Tests And Verification

- Add storage unit coverage that opens a read statement over a multi-page
  catalog, corrupts an overflow page on disk, and verifies the active read
  statement still serves metadata from its validated snapshot image.
- Verify the same corrupted file is rejected after the read statement closes,
  proving the test exercised the cache rather than weakening validation.
- Run focused storage tests, storage-smoke compatibility groups, formatting
  checks, and `git diff --check`.

## Acceptance Criteria

- `read_catalog_image()` can return a matching statement/read checkpoint
  catalog image without rereading overflow pages.
- Catalog cache invalidation remains tied to catalog root page and generation.
- Read-statement snapshot semantics cover multi-page catalogs.
- Existing storage and routed DDL/DML compatibility tests pass.

## Risks

- This remains a whole-image metadata cache. It avoids repeat chain I/O, but
  very large catalogs still copy the full image into each mutable caller.
- The durable non-statement path still validates from disk. A future pager can
  own broader validated-page lifetimes.

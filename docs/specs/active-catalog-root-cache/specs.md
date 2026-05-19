# Active Catalog Root Cache

## Problem

After active header reads started returning decoded in-memory headers, the
update benchmark still spends visible time repeatedly validating the catalog
root page while the same active storage checkpoint is in scope. Update and
duplicate-key paths call `read_catalog_root()` to resolve table metadata, and
each call currently re-reads or re-validates the same catalog root page even
when no catalog DDL has occurred.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::update_row()` calls duplicate-key and
  storage update paths inside a MyLite statement checkpoint.
- `packages/mylite-storage/src/storage.c::mylite_storage_find_index_entry()`
  and `mylite_storage_update_row_with_index_entries()` both resolve table ids
  through `read_catalog_root()`.
- `read_checkpoint_snapshot()` already reads and validates the checkpoint
  catalog root before an active statement is installed.
- `write_page_at()` intercepts active header writes, but catalog root writes
  currently go to the primary file immediately and rely on the statement
  rollback journal plus checkpoint catalog snapshot for rollback.
- Catalog writes are concentrated in metadata DDL paths that update the catalog
  checksum before writing the root page and then publish a matching header
  generation.

## Design

- Add a per-active-statement cache of the last validated catalog root page,
  keyed by catalog root page id and catalog generation.
- Populate the cache from `read_checkpoint_snapshot()` after the catalog root
  has been validated.
- Let `read_catalog_root()` return the cached page for active statements when
  the caller's header root page and generation match the cached page.
- Refresh the cache only after `read_catalog_root()` has read and validated a
  catalog page through the existing slow path.
- Invalidate the active statement chain's catalog-root caches when an active
  catalog root page is written, and when an active header write changes catalog
  root page or generation. Do not convert catalog writes into memory-only
  writes in this slice.

## Compatibility Impact

SQL-visible behavior is unchanged. The cache returns catalog pages that have
already passed the same checksum and record validation that `read_catalog_root()`
would perform. DDL metadata routing continues to write catalog pages through
the existing journaled path and invalidates active catalog caches before later
metadata reads.

## Single-File And Lifecycle Impact

No file-format change and no new companion files. The cache is transient
statement memory and is cleared when the statement is committed, rolled back,
or freed.

## Test And Verification Plan

- Existing statement checkpoint tests cover active row reads, catalog DDL
  rollback, and nested statement catalog visibility. With this cache enabled,
  those paths also cover cache invalidation across inner DDL commit and outer
  rollback.
- Rebuild and run the storage unit tests.
- Rebuild the storage-smoke target and run the storage-engine compatibility
  harness.
- Run the local performance baseline to measure update-path impact.
- Run changed-line formatting checks and `git diff --check`.

## Acceptance Criteria

- Active row-DML and duplicate-key paths avoid repeated catalog root checksum
  validation when the catalog generation has not changed.
- Active DDL writes invalidate stale catalog-root caches before later metadata
  reads.
- Storage and storage-engine compatibility tests pass.
- Update-heavy benchmark timings improve or stay within noise without changing
  metadata visibility or rollback behavior.

## Risks

- The cache is intentionally scoped to active statements. Durable non-active
  catalog reads still validate the page every time until a broader pager or
  file-state cache owns validated page lifetimes.

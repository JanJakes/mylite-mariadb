# Table Index-Root Absence Cache

## Problem

After exact-index lookup stopped loading the catalog on active cache hits, the
prepared update profile still shows catalog materialization under
`update_row_with_index_entries()`. The update path reads the catalog before
planning maintained index-root rewrites, even for hot tables that have already
proven they do not have catalog-published index roots.

For the current append-only update benchmark, repeated updates therefore keep
copying metadata solely to rediscover that maintained root planning has no work.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::update_row()` routes MariaDB handler
  row updates to `mylite_storage_update_row_with_index_entry_changes()`.
- `packages/mylite-storage/src/storage.c::update_row_with_index_entries()`
  resolves the table id, reads the catalog image, and calls
  `plan_maintained_index_root_updates()` before starting the write journal.
- `plan_maintained_index_root_updates()` only needs the catalog to find
  `MYLITE_STORAGE_FORMAT_RECORD_TYPE_INDEX_ROOT` records for the updated table.
- Catalog writes already advance `catalog_generation` or `catalog_root_page`, so
  an absence cache keyed by those values becomes stale automatically when index
  root metadata changes.
- A sampled `mylite_perf_baseline --phase=prepared-updates 1000 1000000` run
  after deferred exact-index catalog loading measured prepared updates at
  `4.453 us/op` and showed `read_catalog_image()` under
  `update_row_with_index_entries()` as the next visible storage-side catalog
  cost.

## Design

- Add one transient per-statement cache entry recording that a table has no
  index-root catalog records for a specific `(catalog_root_page,
  catalog_generation, table_id)`.
- Check that absence cache before reading the catalog for maintained update
  planning.
- If the cache says no index roots exist for the table, leave the maintained
  update plan empty and continue through the existing append-buffer or inline
  update paths.
- When the catalog is read and no index-root records exist for the table, store
  the absence entry on the active cache statement.
- If the catalog contains any index-root record for the table, do not store an
  absence entry. Existing maintained-root planning remains authoritative.
- Keep positive index-root entry caching unchanged.

## Affected Subsystems

- MyLite storage row-update planning.
- Maintained index-root metadata checks.
- Storage-smoke update performance baseline.

## Compatibility Impact

No SQL-visible, handler, public API, or MySQL/MariaDB compatibility behavior
changes. The cache only skips maintained-root planning after the same catalog
generation has already proven that the table has no root records.

## Single-File And Lifecycle Impact

No durable file-format, companion-file, lock, journal, recovery, or lifecycle
change. The cache is transient statement memory and is invalidated by catalog
root or generation changes.

## Public API And File-Format Impact

No public `libmylite` API, internal storage API, or file-format change.

## Binary-Size And Dependency Impact

Small first-party C change. No new dependency.

## Tests And Verification

- Rebuild `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline`.
- Run focused storage and embedded storage-engine CTest groups.
- Run the focused prepared-update performance baseline.
- Sample the one-million prepared-update benchmark and confirm the hot catalog
  frame under `update_row_with_index_entries()` moves down or disappears for
  no-root tables.
- Run `git diff --check`.
- Run `git clang-format --diff` on the touched C file.

Verification after implementation:

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`
- Sampled one-million prepared-update benchmark with macOS `sample`.
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`

Measured rerun:

- Prepared primary-key updates: `3.449 us/op`.

Sampled rerun:

- The sampled update-planning path no longer showed `read_catalog_image()` as a
  hot frame under `update_row_with_index_entries()` for the no-root table.
- The next visible storage-side cost is active row-payload cache checksum
  validation during indexed-row materialization.

## Acceptance Criteria

- Repeated updates on a table with no catalog-published index roots skip
  catalog-image materialization for maintained-root planning.
- Tables with any matching index-root catalog record keep the existing planning
  path.
- Catalog generation/root changes make cached absence entries miss.
- Existing storage and embedded storage-engine tests remain green.

## Risks And Open Questions

- This is a negative cache for the common no-root table shape. Tables with
  maintained roots still pay catalog planning cost until broader root metadata
  caches are introduced.
- Row-payload checksum validation remains a separate hot path.

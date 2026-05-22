# Additive Table Catalog Cache Retarget

## Problem

The prepared-update benchmark builds a durable exact-index cache for
`perf_rows` during the initial insert transaction, then creates and populates
`perf_prepared_rows` before updating `perf_rows`. `CREATE TABLE
perf_prepared_rows` publishes catalog-only table and schema metadata changes
and currently clears the file's durable exact-index caches. The first prepared
update on `perf_rows` therefore has to rebuild a complete active exact-index
cache from the append history even though `perf_rows` rows and indexes did not
change.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc::execute_statement()` wraps prepared
  row-DML in storage checkpoints when a file-backed transaction is active.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::direct_update_rows()` reads
  the target row through `read_exact_unique_index_row_into()` before calling
  `update_row()`.
- `packages/mylite-storage/src/storage.c::mylite_storage_store_table_definition()`
  rejects existing table names, allocates a new table id, appends a table
  record, writes the definition BLOB pages, and publishes the new catalog image.
- `packages/mylite-storage/src/storage.c::mylite_storage_store_schema()` and
  `mylite_storage_store_schema_definition()` publish schema metadata records
  without changing existing table ids, table definitions, or row/index bytes.
- `packages/mylite-storage/src/storage.c::publish_catalog_image()` clears
  active catalog-dependent caches and durable exact-index caches after every
  successful catalog publish because DDL can change table ids, index
  definitions, and row layouts.
- Durable live-row-id, row-payload, and exact-index caches include table id and
  committed header identity. Existing row-DML retargeting already updates
  unaffected table-local caches to a new header fingerprint after unrelated row
  mutations.
- The prepared update path may pass a sparse changed-index list for accepted
  direct updates. In that shape, an omitted index is known unchanged by the
  handler's write-set analysis; active exact-index caches for omitted indexes
  must retarget row ids rather than clear the whole active cache.

## Design

- Keep broad invalidation as the default catalog-publish behavior.
- Add a narrow catalog-publish cache action for additive table creation and
  schema metadata publication. `mylite_storage_store_table_definition()` is
  eligible because it fails if the table already exists and appends a new table
  record instead of changing any existing table definition. Schema record
  publication is eligible because existing table metadata and row/index storage
  remain unchanged.
- For eligible catalog publications:
  - clear active catalog-dependent caches for the file, because the active
    statement's catalog view changed;
  - retarget durable live-row-id, row-payload, and exact-index caches for the
    same file to the new committed header identity;
  - keep durable index-leaf page caches cleared for now, because leaf-root
    metadata is catalog-keyed and the current row-DML helper already chooses
    the conservative path there.
- If an eligible catalog publication occurs under an active storage statement,
  clear active catalog-dependent caches immediately but defer durable cache
  retargeting until the top-level statement commits. Statement rollback leaves
  the durable caches keyed to the still-current pre-statement header.
- Do not preserve durable caches across schema drop, table drop, table rename,
  FK metadata writes, index-root publication, index-root drop, index-leaf
  rebuild publication, truncate, rollback, or copy-rebuild DDL.
- When an update supplies an `index_entry_changed` bitmap and omits an active
  exact-index cache's index, treat the cached index key as unchanged and
  retarget matching cached row ids from old row id to new row id. Keep the
  previous full-cache clear for calls without the changed-entry bitmap, where
  omission does not carry that handler proof.

## Affected Subsystems

- MyLite storage catalog publication.
- Durable process-local storage caches.
- Prepared primary-key update performance after unrelated table creation.

No SQL parser, handler routing, public API, wire protocol, or file-format
change is intended.

## Compatibility Impact

No SQL-visible behavior change is intended. Existing tables keep the same table
ids, row payloads, and index entries when another table is created in the same
catalog. The change only avoids discarding transient process-local caches for
those unaffected tables.

## Single-File And Embedded Lifecycle Impact

No file-format, journal, sidecar, lock, or embedded open/close lifecycle change.
The preserved caches remain process-local and are still keyed to the committed
`.mylite` file header after retargeting.

## Public API And File-Format Impact

No public `libmylite`, MyLite storage C API, or `.mylite` file-format change.

## Binary-Size And Dependency Impact

Small first-party storage-control-flow change only; no new dependency or
embedded profile change is expected.

## Tests And Verification

- Added storage unit coverage that builds a durable exact-index cache for one
  table, publishes schema metadata and a second table, verifies the durable
  exact cache remains a single usable cache, confirms active-statement rollback
  does not strand the cache on an uncommitted header, and confirms a later exact
  lookup does not create a second stale-header cache.
- Added storage unit coverage that seeds an active primary-key exact cache,
  performs a secondary-key update through a sparse changed-entry list, and
  verifies the active primary-key cache remains live and points at the updated
  row id.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage.capabilities'
  --output-on-failure`
  - 1/1 test passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - 2/2 tests passed.
- `ctest --preset storage-smoke-dev --output-on-failure`
  - 10/10 tests passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-update-components 1000 10000`
  - `prepared primary-key update bind component`: `0.019 us/op`
  - `prepared primary-key update step component`: `2.148 us/op`
  - `prepared primary-key update reset component`: `0.019 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-update-components 10000 100000`
  - `prepared primary-key update bind component`: `0.021 us/op`
  - `prepared primary-key update step component`: `2.146 us/op`
  - `prepared primary-key update reset component`: `0.021 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-update-components 10000 1000000`
  - `prepared primary-key update bind component`: `0.022 us/op`
  - `prepared primary-key update step component`: `1.707 us/op`
  - `prepared primary-key update reset component`: `0.022 us/op`

## Acceptance Criteria

- New-table and schema metadata catalog publication preserve durable
  table-local caches for unrelated existing tables by retargeting them to the
  new header identity.
- Existing broad invalidation behavior remains for catalog changes that can
  alter, remove, rename, or otherwise reinterpret existing table metadata.
- Exact-index lookups after unrelated table creation return the same row id
  without accumulating a second durable exact-index cache for the old header.
- Active-statement rollback after an additive catalog publication leaves
  durable caches usable against the committed pre-statement header.
- Sparse changed-entry updates retarget active exact caches for omitted
  unchanged indexes instead of discarding them.
- Storage and embedded smoke tests pass.
- Local prepared-update component timing improves materially or, if noise hides
  the aggregate result, profiling no longer shows the first update rebuilding a
  complete exact-index cache because of the unrelated table creation.

## Risks And Unresolved Questions

- Preserving caches across broader DDL could be valid for some operations, but
  this slice intentionally avoids that until each operation has table-id and
  index-definition stability coverage.
- Durable index-leaf page caches may also be preservable across additive table
  creation, but keeping them conservative avoids coupling this write-path
  performance slice to catalog-root metadata cache semantics.

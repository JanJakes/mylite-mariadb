# Deferred Exact-Index Catalog Load

## Problem

The prepared primary-key update benchmark now spends visible storage-side time
materializing and copying catalog images during indexed-row lookup. The hot
point-update path normally resolves the row id through the active exact-index
cache, but `find_indexed_row_payload()` still reads the catalog image before
calling `find_exact_index_row_id()`.

That catalog image is only needed when the exact-index cache misses and the
lookup must fall through to published leaf-root access paths. Loading it before
checking the cache makes repeated point updates pay metadata work that does not
affect the result.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::index_read_map()` builds handler index
  cursors over MyLite storage lookups for MariaDB range access.
- `mariadb/storage/mylite/ha_mylite.cc::update_row()` performs the row update
  after the primary-key lookup.
- `packages/mylite-storage/src/storage.c::find_indexed_row_payload()` and the
  row-id-only indexed lookup both read the catalog image even when the active
  table-entry cache has already resolved the table.
- `packages/mylite-storage/src/storage.c::find_exact_index_row_id()` first
  checks the active exact-index cache. On the hot repeated-update path that
  cache owns the answer and no catalog-backed leaf lookup is needed.
- `packages/mylite-storage/src/storage.c::read_catalog_image()` copies cached
  catalog images for caller-owned lifetime, so the unnecessary read still costs
  allocation, copy, and metadata validation work.
- A sampled `mylite_perf_baseline --phase=prepared-updates 1000 1000000` run
  measured prepared updates at `4.301 us/op` and showed
  `read_catalog_image()` below `find_indexed_row_payload()` as a visible
  storage-side frame before update execution.

## Design

- Keep table-id resolution unchanged. If the active table-entry cache misses,
  read the catalog image and store the table-entry cache exactly as today.
- Remove the unconditional catalog read after table-entry resolution from both
  indexed-row lookup entry points.
- Let `find_exact_index_row_id()` own catalog materialization for the paths that
  need it.
- Add a small helper that reads the catalog image only when the caller has not
  already loaded it.
- Preserve lookup order after an exact-index cache miss: published static leaf
  root, published leaf-row-id lookup, durable exact-index cache, then append
  history scan.
- Do not change exact-index cache contents, durable cache promotion, row
  payload caching, catalog cache invalidation, or row visibility rules.

## Affected Subsystems

- MyLite storage exact-index lookup.
- Handler-driven primary-key point reads used by routed updates.
- Storage-smoke performance baseline.

## Compatibility Impact

No SQL-visible, handler, public API, or MySQL/MariaDB compatibility behavior
changes. Cache hits return the same row id, and cache misses still load the
catalog before trying catalog-backed leaf-root paths.

## Single-File And Lifecycle Impact

No durable file-format, companion-file, lock, journal, recovery, or lifecycle
change. The slice only changes when an existing transient catalog image is
materialized.

## Public API And File-Format Impact

No public `libmylite` API, internal storage API, or file-format change.

## Binary-Size And Dependency Impact

Small first-party C change. No new dependency.

## Tests And Verification

- Rebuild `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline`.
- Run storage and embedded storage-engine CTest groups.
- Run the focused prepared-update performance baseline before and after the
  change.
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

- Prepared primary-key updates: `4.453 us/op`.

Sampled rerun:

- The sampled indexed-row lookup no longer showed `read_catalog_image()` under
  `find_indexed_row_payload()` on the active exact-index cache-hit path.
- The next visible catalog hot spot is `read_catalog_image()` under
  `update_row_with_index_entries()` for maintained-root planning.
- Active row-payload cache checksum validation remains visible after catalog
  work.

## Acceptance Criteria

- Exact-index cache hits no longer require catalog-image materialization after
  the table-entry cache is warm.
- Exact-index cache misses preserve existing catalog-backed lookup behavior.
- Existing storage and embedded storage-engine tests remain green.
- Benchmark evidence records the prepared-update impact.

## Risks And Open Questions

- This does not remove catalog work from cold first lookups or cache misses.
- This does not address row-payload checksum validation, which remains visible
  after catalog work in the current sampled profile.

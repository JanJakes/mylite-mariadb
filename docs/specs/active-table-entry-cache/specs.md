# Active Table Entry Cache

## Problem

The routed update benchmark still spends visible time in metadata lookup before
each `UPDATE ... WHERE id = ?` row rewrite. After inline exact unique cursors
and reusable indexed-row buffers, the sampled hot path still shows both
`find_indexed_row_payload()` and write-side `find_table_id()` repeatedly copying
the active catalog root page and scanning catalog records with
`find_table_record()` before the active exact-index and row-update paths can use
the table id.

That table lookup is invariant for a hot row-DML loop while the active
checkpoint already owns the current file view and the catalog root/generation
have not changed.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::build_index_cursor()` calls
  `mylite_storage_find_indexed_row_reuse()` for non-BLOB durable exact unique
  cursor construction.
- `packages/mylite-storage/src/storage.c::find_indexed_row_payload()` reads the
  current header, reads the catalog root page, calls `find_table_record()`, then
  calls `find_exact_index_row_id()`.
- `packages/mylite-storage/src/storage.c::update_row_with_index_entries()` calls
  `find_table_id()` before the row rewrite. That helper previously repeated the
  same catalog-root copy and `find_table_record()` scan.
- `read_header()` and `read_catalog_root()` already reuse the active statement's
  current header/catalog bytes when the call runs under the same storage owner,
  but `read_catalog_root()` still copies a full page and `find_table_record()`
  still scans catalog records for every row lookup.
- `find_exact_index_row_id()` checks the active exact-index cache before using
  published leaf roots or durable scans, so active row-DML point lookups only
  need a trusted table id to reach the cache path.
- The latest one-million direct-update sample records visible samples in
  `read_catalog_root()` and `find_table_record()` under
  `mylite_storage_find_indexed_row_reuse()`.

## Design

- Add one active-statement table-entry cache to the storage statement object.
- Cache only schema name, table name, catalog root page, catalog generation,
  table id, definition root page, and definition size.
- Do not cache a catalog-record pointer. Cached entries must set
  `record = NULL` because the original pointer refers to a caller stack copy of
  the catalog page.
- Probe the active-statement cache after `read_header()` and before
  `read_catalog_root()` in `find_indexed_row_payload()`.
- Probe the same cache in `find_table_id()` before reading the catalog root, and
  fill it from the normal catalog path on miss.
- On cache hit, skip catalog-root copy and table-record scan, then call the
  exact-index lookup with no catalog page. This is restricted to active
  checkpoints where the active exact-index cache path is the first lookup path.
- On cache miss, keep the existing catalog-root and table-record path, then
  store the table entry on successful lookup.
- Clear the cache with existing statement catalog invalidation and statement
  cleanup. The catalog root page and catalog generation remain part of the
  cache key so cache hits are tied to the same active catalog view.

## Affected Subsystems

- MyLite storage active checkpoint state.
- Row insert, update, delete, truncate, and autoincrement paths that resolve
  table ids through `find_table_id()`.
- Durable handler exact unique cursor construction.
- Storage-smoke update performance baseline.

## Compatibility Impact

No SQL or MySQL/MariaDB compatibility behavior changes. The cache is internal
process memory and only bypasses repeated metadata lookup for an already-active
file-owned checkpoint view.

## Single-File And Lifecycle Impact

No durable file-format, companion-file, lock, transaction, journal, or recovery
change. The cache is owned by an active storage statement and disappears when
the statement closes.

## Public API And File-Format Impact

No public `libmylite` API, internal storage API, or file-format change.

## Binary-Size And Dependency Impact

Small first-party C change. No new dependency. Each active storage statement
gains one bounded table-entry cache with copied schema/table names.

## Tests And Verification

- Rebuild the MariaDB storage-smoke embedded archive with
  `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build
  all -DPLUGIN_MYLITE_SE=STATIC`.
- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline` with the `storage-smoke-dev` preset.
- Run `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Run `ctest --test-dir build/storage-smoke-dev --output-on-failure`.
- Run `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=updates 1000
  1000000`.
- Run a sampled one-million-update benchmark and confirm the
  `find_indexed_row_payload()` frame no longer shows repeated
  `read_catalog_root()` / `find_table_record()` work on the active exact unique
  path.
- Run `git diff --check` and `git clang-format --diff` on touched C files.

Verification after implementation:

- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build
  all -DPLUGIN_MYLITE_SE=STATIC` rebuilt the storage-smoke embedded archive at
  `20.07 MiB`.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev --output-on-failure` passed 10/10.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=updates 1000
  1000000`
- sampled one-million-update benchmark with macOS `sample`
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c
  packages/mylite-storage/tests/storage_test.c`

Measured update baseline:

- Direct primary-key updates: `11.370 us/op`.
- Prepared primary-key updates: `4.890 us/op`.

Sampled rerun:

- Direct primary-key updates: `11.518 us/op`.
- Prepared primary-key updates: `4.948 us/op`.

Final verification rerun:

- Direct primary-key updates: `11.643 us/op`.
- Prepared primary-key updates: `5.006 us/op`.

The sampled direct-update frame still shows `find_table_id()` and
`find_indexed_row_payload()` control-plane cost, but those frames now use
`find_active_table_entry_cache()` instead of showing repeated
`read_catalog_root()` and `find_table_record()` under the hot update and exact
indexed-row lookup paths. Remaining visible storage work includes active row
payload cache probing, live-row validation cache maintenance, exact-index cache
lookup, and active buffered rewrite work.

## Acceptance Criteria

- Active exact unique cursor construction and write-side table-id resolution can
  reuse a cached table id when the schema/table and catalog root/generation are
  unchanged.
- Cached table entries never expose stale catalog-record pointers.
- Catalog changes, statement close, and ordinary cache misses preserve the
  existing catalog validation and table-record lookup path.
- Existing storage and embedded storage-engine tests remain green.
- Benchmark/profile evidence records whether the targeted metadata work moved
  wall-clock update latency.

## Risks And Open Questions

- This removes only control-plane metadata work from the active point-lookup
  path. Current profiles also show active update rewrite, exact-index cache
  maintenance, MariaDB parsing/execution, and prepared binding/reset costs.
- A broader durable table-entry cache for non-active read statements may be
  useful later, but this slice avoids changing published-leaf lookup ordering
  outside active checkpoints.

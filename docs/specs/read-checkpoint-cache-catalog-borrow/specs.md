# Read Checkpoint Cache Catalog Borrow

## Problem

The storage read-statement benchmark shows `3.910 us/op` for hot begin/end
pairs. A hot read statement that hits the read-checkpoint cache still
deep-copies the cached catalog image into the short-lived statement, then frees
that copy when the statement ends.

That copy is unnecessary for point lookup helpers that only need a stable
catalog view during one storage call.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `packages/mylite-storage/src/storage.c::read_cached_checkpoint_snapshot()`
  validates the current header page and copies the read-checkpoint cache into
  the new read statement on a hot hit.
- `copy_read_checkpoint_cache_to_statement()` deep-copies
  `active_read_checkpoint_cache.current_catalog_image`.
- `catalog_image_view_for_file()` already supports borrowed catalog views from
  active statements for exact lookup helpers.
- If a borrowed view is unavailable, `read_catalog_image()` can still create an
  owned catalog image from the locked file view.

## Design

- Stop deep-copying the cached catalog image into every read statement on a
  read-checkpoint cache hit.
- Let `borrow_current_catalog_image_for_file()` borrow the read-checkpoint
  cache catalog image through the active read statement when the cache identity,
  root page, and generation match.
- Let owned catalog-image reads copy from the read-checkpoint cache when a
  caller needs an owned image and the cache still matches.
- Keep header page, catalog root page, and decoded header copying unchanged.
- Preserve fallback behavior: if the global read-checkpoint cache is replaced
  while a read statement remains active, later catalog-image requests read from
  the locked file view.

## Compatibility Impact

No SQL-visible behavior changes. The borrowed image is immutable catalog state
validated against the same header root and generation.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle change.
Read statements still acquire and release the same shared lock.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

Durable routed table reads can avoid one transient catalog-image allocation on
hot read-checkpoint cache hits. Volatile MEMORY/HEAP reads are unaffected.

## Binary-Size And Dependency Impact

No dependency change. Runtime allocation pressure drops for hot short read
statements.

## Tests And Verification

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build libmariadbd.a`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-read-statements 10000 1000000`
  - `storage read statement begin/end pairs`: `3.729 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-pk-entry-lookups 10000 1000000`
  - `storage primary-key entry lookups`: `4.086 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-pk-row-lookups 10000 1000000`
  - `storage primary-key row lookups`: `4.584 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-pk-entry-lookups-one-read 10000 1000000`
  - `storage primary-key entry lookups in one read statement`: `0.174 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-pk-row-lookups-one-read 10000 1000000`
  - `storage primary-key row lookups in one read statement`: `0.511 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-selects 10000 1000000`
  - `prepared primary-key point selects`: `7.635 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-scalar-selects 10000 1000000`
  - `prepared scalar selects`: `0.681 us/op`
- `git diff --check`
- `git clang-format --diff -- packages/mylite-storage/src/storage.c`

## Acceptance Criteria

- Hot read-checkpoint cache hits no longer deep-copy catalog images into the
  short-lived read statement.
- Exact lookup helpers can still borrow a catalog image during the active read
  statement.
- Owned catalog-image callers can still copy from the matching read-checkpoint
  cache.
- Existing storage and storage-engine smoke tests pass.
- The local performance baseline records the read-statement and point-lookup
  effect.

## Risks And Unresolved Questions

- The borrowed cache image is process-local and can disappear if another file
  replaces the read-checkpoint cache. The implementation must always fall back
  to an owned catalog read in that case.

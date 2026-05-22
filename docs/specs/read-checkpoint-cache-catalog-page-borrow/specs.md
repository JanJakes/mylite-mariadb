# Read Checkpoint Cache Catalog Page Borrow

## Problem

After catalog-image borrowing, hot read statements still copy cached catalog
root/current page buffers into every short-lived statement on read-checkpoint
cache hits. Exact point lookup helpers usually borrow the cached catalog image
and do not need those page buffers.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `copy_read_checkpoint_cache_to_statement()` copied the cached header page,
  catalog page, and current catalog page into every hot read statement.
- `read_catalog_root()` can serve catalog root bytes from an active statement's
  current catalog page when needed.
- `active_read_checkpoint_cache` already retains the same current catalog page
  and identity metadata for the matched file/header generation.

## Design

- On read-checkpoint cache hits, keep copying decoded header state and the
  header page, but do not copy catalog root/current page buffers into the new
  read statement.
- Let `read_catalog_root()` copy the catalog page from the matching
  read-checkpoint cache when an active read statement has no local page copy.
- Preserve the existing fallback to reading and validating the catalog root page
  from the locked file view.
- Leave cache misses unchanged: a read statement that has just read the catalog
  from disk still owns its materialized page/image state.

## Compatibility Impact

No SQL-visible behavior changes. The optimization only changes where transient
catalog page bytes are copied from on hot read-cache hits.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle change.
Read statements still acquire and release the same shared lock.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

Durable routed point reads avoid transient catalog page copies on hot
read-checkpoint cache hits. Volatile MEMORY/HEAP reads are unaffected.

## Binary-Size And Dependency Impact

No dependency change. Runtime memory-copy volume drops for hot short read
statements.

## Tests And Verification

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build libmariadbd.a`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-read-statements 10000 1000000`
  - `storage read statement begin/end pairs`: `3.706 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-pk-entry-lookups 10000 1000000`
  - `storage primary-key entry lookups`: `3.994 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-pk-row-lookups 10000 1000000`
  - `storage primary-key row lookups`: `4.444 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-pk-entry-lookups-one-read 10000 1000000`
  - `storage primary-key entry lookups in one read statement`: `0.180 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-pk-row-lookups-one-read 10000 1000000`
  - `storage primary-key row lookups in one read statement`: `0.546 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-selects 10000 1000000`
  - `prepared primary-key point selects`: `7.570 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-scalar-selects 10000 1000000`
  - `prepared scalar selects`: `0.709 us/op`
- `git diff --check`
- `git clang-format --diff -- packages/mylite-storage/src/storage.c`

## Acceptance Criteria

- Hot read-checkpoint cache hits no longer copy catalog page buffers into every
  short-lived read statement.
- Catalog root callers can still obtain a validated page from the active
  statement, read-checkpoint cache, or locked file fallback.
- Existing storage and storage-engine smoke tests pass.
- The local performance baseline records the read-statement and point-lookup
  effect.

## Risks And Unresolved Questions

- If the read-checkpoint cache is replaced while a read statement is still
  active, catalog root callers fall back to the locked file view. This matches
  the catalog-image borrow fallback and should remain covered by storage tests.
